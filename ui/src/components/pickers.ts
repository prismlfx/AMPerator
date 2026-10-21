// Modal pickers used by the pedalboard: NAM model / IR, plugin (grouped, with search and rescan) and built-in art.

import { call } from '../bridge';
import { confirmDialog, el, openModal, toast } from '../dialog';
import { patchNode, type RigNode } from '../state';
import { ARCHETYPES, archetypeLabel, artKind } from '../art/archetypes';
import { filterPlugins, groupPlugins, type Group, type GroupId, type PluginInfo } from '../pluginCategories';

const showError = (e: unknown) => toast((e as Error).message ?? String(e));

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

const RESCAN_TIMEOUT_MS = 30 * 60 * 1000; // the engine answers when the scan is done
const GUITAR_ONLY_KEY = 'amperator.pluginPicker.guitarOnly';
const COLLAPSED_BY_DEFAULT: GroupId[] = ['utility', 'instrument'];

/** The Guitar toggle's last state (on unless switched off; storage may be unavailable). */
function loadGuitarOnly(): boolean {
  try {
    return localStorage.getItem(GUITAR_ONLY_KEY) !== '0';
  } catch {
    return true;
  }
}

function saveGuitarOnly(on: boolean) {
  try {
    localStorage.setItem(GUITAR_ONLY_KEY, on ? '1' : '0');
  } catch {
    // not remembered then
  }
}

const formatBadge = (format: string) => (format === 'AudioUnit' ? 'AU' : format);

/** The plugin picker: search, a Guitar toggle (hides Utility & Other and Instruments), Rescan, and the scanned plugins
    in groups (pluginCategories.ts). Choosing one adds a Plugin node at chain position `index`. */
export function openPluginPicker(index: number) {
  const modal = openModal('Add a plugin');
  let plugins: PluginInfo[] | null = null; // null until listPlugins replies
  let busy = false;
  let query = '';
  let guitarOnly = loadGuitarOnly();
  const collapsed = new Set<GroupId>(COLLAPSED_BY_DEFAULT);
  const collapsedWhileSearching = new Set<GroupId>(); // groups are expanded while searching unless collapsed then
  const isCollapsed = (id: GroupId) => (query.trim() ? collapsedWhileSearching : collapsed).has(id);

  const choose = (p: PluginInfo) => {
    modal.close();
    const plugin = { format: p.format, uid: p.uid, name: p.name, vendor: p.vendor }; // the PluginRef part
    call('addNode', { type: 'Plugin', index, plugin }).catch(showError);
  };

  // The toolbar stays in the DOM while the list below re-renders (keeps the focus in the search field).
  const search = el('input', {
    class: 'library-search plugin-search',
    type: 'search',
    placeholder: 'Search plugins…',
    'aria-label': 'Search plugins',
    spellcheck: 'false',
    autocomplete: 'off',
  });
  search.addEventListener('input', () => {
    query = search.value;
    collapsedWhileSearching.clear();
    render();
  });
  search.addEventListener('keydown', (e) => {
    if (e.key !== 'Enter') return;
    e.preventDefault();
    list.querySelector<HTMLButtonElement>('.plugin-list:not([hidden]) .plugin-row:not(:disabled)')?.click();
  });
  const guitar = el(
    'button',
    { class: 'tag-filter plugin-guitar-toggle', type: 'button', title: 'Show only plugins for a guitar chain' },
    'Guitar',
  );
  guitar.addEventListener('click', () => {
    guitarOnly = !guitarOnly;
    saveGuitarOnly(guitarOnly);
    render();
  });
  const rescan = el('button', { class: 'button small', type: 'button', title: 'Rescan plugins' }, 'Rescan');
  rescan.addEventListener('click', () => void runRescan());
  const list = el('div', { class: 'plugin-groups' });
  modal.body.append(el('div', { class: 'plugin-toolbar' }, search, guitar, rescan), list);

  async function runRescan() {
    const ok = await confirmDialog(
      'Scanning checks every VST3 and Audio Unit plugin on this Mac. It may take a while, and AMPerator stops responding until it is done.',
      'Scan now',
    );
    if (!ok) return;
    busy = true;
    render();
    try {
      const r = await call<{ plugins: PluginInfo[] }>('rescanPlugins', undefined, RESCAN_TIMEOUT_MS);
      plugins = r.plugins;
    } catch (e) {
      showError(e);
    } finally {
      busy = false;
      render();
    }
  }

  function renderGroup(group: Group, members: PluginInfo[]): HTMLElement {
    const open = !isCollapsed(group.id);
    const head = el(
      'button',
      { class: 'plugin-group-header', type: 'button', 'aria-expanded': String(open) },
      el('span', { class: 'chevron', 'aria-hidden': 'true' }, open ? '▾' : '▸'),
      el('span', { class: 'plugin-group-label' }, group.label),
      el('span', { class: 'plugin-group-count' }, String(members.length)),
    );
    head.addEventListener('click', () => {
      const set = query.trim() ? collapsedWhileSearching : collapsed;
      if (set.has(group.id)) set.delete(group.id);
      else set.add(group.id);
      render();
    });
    const rows = el(
      'ul',
      { class: 'plugin-list' },
      ...members.map((p) => {
        const b = el(
          'button',
          { class: 'list-button plugin-row', type: 'button', title: `${p.name} (${p.vendor})` },
          el('span', { class: 'plugin-name' }, p.name),
          el('span', { class: 'muted plugin-vendor' }, p.vendor),
          el('span', { class: 'plugin-format' }, formatBadge(p.format)),
        );
        b.disabled = busy;
        b.addEventListener('click', () => choose(p));
        return el('li', {}, b);
      }),
    );
    rows.hidden = !open;
    return el('section', { class: 'plugin-group', 'data-group': group.id }, head, rows);
  }

  function render() {
    guitar.setAttribute('aria-pressed', String(guitarOnly));
    rescan.disabled = busy;
    rescan.textContent = busy ? 'Scanning…' : 'Rescan';
    if (plugins === null) {
      list.replaceChildren(el('p', { class: 'muted' }, 'Loading…'));
      return;
    }
    if (busy) {
      list.replaceChildren(el('p', { class: 'muted' }, 'Scanning plugins…'));
      return;
    }
    if (plugins.length === 0) {
      const again = el('button', { class: 'link-button', type: 'button' }, 'Rescan');
      again.addEventListener('click', () => void runRescan());
      list.replaceChildren(el('p', { class: 'muted plugin-empty' }, 'No plugins scanned yet — ', again));
      return;
    }
    const shown = groupPlugins(filterPlugins(plugins, query, guitarOnly)).filter(({ group }) => group.guitar || !guitarOnly);
    list.replaceChildren(
      ...(shown.length === 0
        ? [el('p', { class: 'muted plugin-empty' }, 'No plugins match')]
        : shown.map(({ group, plugins: members }) => renderGroup(group, members))),
    );
  }

  render();
  search.focus();
  call<{ plugins: PluginInfo[] }>('listPlugins')
    .then((r) => {
      plugins = r.plugins;
      render();
    })
    .catch((e) => {
      modal.close();
      showError(e);
    });
}

/** `title` is the node title shown on its card. */
export function openArtDialog(n: RigNode, title: string) {
  const kind = artKind(n.type);
  const current = (n.art as { archetype?: string } | undefined)?.archetype ?? '';
  const modal = openModal(`Art for ${title}`, undefined, 'modal-small');
  const select = el('select', { 'aria-label': 'Built-in art' }, el('option', { value: '' }, 'Automatic'));
  for (const a of ARCHETYPES[kind]) select.append(el('option', { value: a }, archetypeLabel(a)));
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
