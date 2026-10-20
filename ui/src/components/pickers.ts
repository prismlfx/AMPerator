// Modal pickers used by the pedalboard: NAM model / IR, plugin (with rescan) and built-in art.

import { call } from '../bridge';
import { confirmDialog, el, openModal, toast } from '../dialog';
import { patchNode, type RigNode } from '../state';

const showError = (e: unknown) => toast((e as Error).message ?? String(e));

/** Built-in art archetypes (spec §6; no brand names). The art itself arrives with the stage view. */
const ARCHETYPES = {
  amp: ['british-stack', 'american-clean-combo', 'high-gain-rectifier', 'ac-chime', 'generic'],
  cab: ['4x12', '2x12', '1x12-open'],
  pedal: ['drive', 'fuzz', 'delay', 'chorus', 'reverb', 'eq', 'gate', 'generic'],
};

interface Asset {
  file: string;
  name: string;
  kind: string;
}

/** Lists the library's models / IRs and assigns the chosen one to node `id`. Resolves when the picker closes. */
export function pickAsset(id: string, kind: 'nam' | 'ir'): Promise<void> {
  return new Promise((resolve) => {
    const modal = openModal(kind === 'nam' ? 'Choose a NAM model' : 'Choose a cabinet IR', resolve);
    modal.body.append(el('p', { class: 'muted' }, 'Loading…'));
    call<{ assets: Asset[] }>('listAssets', { kind })
      .then(({ assets }) => {
        const choose = (file: string) => {
          modal.close();
          call('setAsset', { id, file }).catch(showError);
        };
        const none = el('button', { class: 'button small' }, kind === 'nam' ? 'No model' : 'No IR');
        none.addEventListener('click', () => choose(''));
        const list =
          assets.length === 0
            ? el(
                'p',
                { class: 'muted' },
                kind === 'nam'
                  ? 'No models yet. Put .nam files in ~/Music/AMPerator/Models, or add a watched folder in Settings.'
                  : 'No IRs yet. Put .wav files in ~/Music/AMPerator/IRs, or add a watched folder in Settings.',
              )
            : el(
                'ul',
                { class: 'asset-list' },
                ...assets.map((a) => {
                  const b = el('button', { class: 'list-button', title: a.file }, el('span', {}, a.name), el('span', { class: 'muted path' }, a.file));
                  b.addEventListener('click', () => choose(a.file));
                  return el('li', {}, b);
                }),
              );
        modal.body.replaceChildren(list, el('div', { class: 'modal-actions' }, none));
      })
      .catch((e) => {
        modal.close();
        showError(e);
      });
  });
}

interface PluginRef {
  format: string;
  uid: string;
  name: string;
  vendor: string;
}

const RESCAN_TIMEOUT_MS = 30 * 60 * 1000; // the engine answers when the scan is done

export function openPluginPicker(index: number) {
  const modal = openModal('Add a plugin');
  let busy = false;

  const show = (plugins: PluginRef[]) => {
    const rescan = el('button', { class: 'button' }, busy ? 'Scanning…' : 'Rescan plugins');
    rescan.disabled = busy;
    rescan.addEventListener('click', async () => {
      const ok = await confirmDialog(
        'Scanning checks every VST3 and Audio Unit plugin on this Mac. It may take a while, and AMPerator stops responding until it is done.',
        'Scan now',
      );
      if (!ok) return;
      busy = true;
      show(plugins);
      try {
        const r = await call<{ plugins: PluginRef[] }>('rescanPlugins', undefined, RESCAN_TIMEOUT_MS);
        plugins = r.plugins;
      } catch (e) {
        showError(e);
      } finally {
        busy = false;
        show(plugins);
      }
    });
    const list =
      plugins.length === 0
        ? el('p', { class: 'muted' }, busy ? 'Scanning plugins…' : 'No plugins found yet. Scan to find the plugins installed on this Mac.')
        : el(
            'ul',
            { class: 'asset-list plugin-list' },
            ...plugins.map((p) => {
              const b = el('button', { class: 'list-button' }, el('span', {}, p.name), el('span', { class: 'muted' }, `${p.vendor} · ${p.format}`));
              b.disabled = busy;
              b.addEventListener('click', () => {
                modal.close();
                call('addNode', { type: 'Plugin', index, plugin: p }).catch(showError);
              });
              return el('li', {}, b);
            }),
          );
    modal.body.replaceChildren(list, el('div', { class: 'modal-actions' }, rescan));
  };

  modal.body.append(el('p', { class: 'muted' }, 'Loading…'));
  call<{ plugins: PluginRef[] }>('listPlugins')
    .then((r) => show(r.plugins))
    .catch((e) => {
      modal.close();
      showError(e);
    });
}

/** `title` is the node title shown on its card. */
export function openArtDialog(n: RigNode, title: string) {
  const kind = n.type === 'NamAmp' ? 'amp' : n.type === 'CabIR' ? 'cab' : 'pedal';
  const current = (n.art as { archetype?: string } | undefined)?.archetype ?? '';
  const modal = openModal(`Art for ${title}`, undefined, 'modal-small');
  const select = el('select', { 'aria-label': 'Built-in art' }, el('option', { value: '' }, 'Automatic'));
  for (const a of ARCHETYPES[kind]) select.append(el('option', { value: a }, a));
  select.value = current;
  const save = el('button', { class: 'button primary' }, 'Save');
  const cancel = el('button', { class: 'button' }, 'Cancel');
  cancel.addEventListener('click', () => modal.close());
  save.addEventListener('click', async () => {
    try {
      const r = await call<{ id: string; art: unknown }>('setNodeArt', { id: n.id, archetype: select.value });
      patchNode(n.id, { art: r.art });
      modal.close();
    } catch (e) {
      showError(e);
    }
  });
  modal.body.append(
    el('label', { class: 'field' }, el('span', { class: 'field-label' }, 'Built-in art'), select),
    el('div', { class: 'modal-actions' }, cancel, save),
  );
}
