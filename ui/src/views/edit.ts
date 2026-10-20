// Edit view (pedalboard): the rig's nodes as pedal / amp / cab cards in signal order (Input and Output are not
// shown). Click a card to show its knobs, drag to reorder, + to add, right-click or ⋯ for the card menu
// (footswitch learn, art, plugin editor, remove). Placeholder (missing) nodes are drawn bypassed, without knobs.

import { call } from '../bridge';
import { createKnob, throttle, type Knob } from '../components/knob';
import { confirmDialog, el, openMenu, openModal, toast, type MenuItem } from '../dialog';
import { patchNode, setLearning, store, type AppState, type Mapping, type RigNode } from '../state';

export interface ParamSpec {
  name: string;
  label: string;
  min: number;
  max: number;
  defaultValue: number;
  unit: string;
  toggle?: boolean;
}

/** Knobs per node type: engine parameter names (engine/dsp), ranges, units and the engine's defaults. */
export const PARAMS: Record<string, ParamSpec[]> = {
  Gate: [
    { name: 'threshold', label: 'Threshold', min: -90, max: 0, defaultValue: -55, unit: 'dB' },
    { name: 'release', label: 'Release', min: 5, max: 1000, defaultValue: 100, unit: 'ms' },
  ],
  EQ: [
    { name: 'low', label: 'Low', min: -12, max: 12, defaultValue: 0, unit: 'dB' },
    { name: 'mid', label: 'Mid', min: -12, max: 12, defaultValue: 0, unit: 'dB' },
    { name: 'high', label: 'High', min: -12, max: 12, defaultValue: 0, unit: 'dB' },
    { name: 'midFreq', label: 'Mid freq', min: 200, max: 5000, defaultValue: 800, unit: 'Hz' },
  ],
  NamAmp: [
    { name: 'inputGain', label: 'Input', min: -24, max: 24, defaultValue: 0, unit: 'dB' },
    { name: 'outputGain', label: 'Output', min: -24, max: 24, defaultValue: 0, unit: 'dB' },
    { name: 'normalize', label: 'Normalize', min: 0, max: 1, defaultValue: 1, unit: '', toggle: true },
  ],
  CabIR: [
    { name: 'lowCut', label: 'Low cut', min: 20, max: 500, defaultValue: 80, unit: 'Hz' },
    { name: 'highCut', label: 'High cut', min: 1000, max: 20000, defaultValue: 8000, unit: 'Hz' },
    { name: 'level', label: 'Level', min: -24, max: 12, defaultValue: 0, unit: 'dB' },
  ],
  Output: [{ name: 'level', label: 'Level', min: -24, max: 12, defaultValue: 0, unit: 'dB' }],
};

/** Built-in art archetypes (spec §6; no brand names). The art itself arrives with the stage view. */
const ARCHETYPES = {
  amp: ['british-stack', 'american-clean-combo', 'high-gain-rectifier', 'ac-chime', 'generic'],
  cab: ['4x12', '2x12', '1x12-open'],
  pedal: ['drive', 'fuzz', 'delay', 'chorus', 'reverb', 'eq', 'gate', 'generic'],
};

const BUILTINS: { type: string; label: string }[] = [
  { type: 'Gate', label: 'Gate' },
  { type: 'EQ', label: 'EQ' },
  { type: 'NamAmp', label: 'NAM Amp' },
  { type: 'CabIR', label: 'Cab IR' },
];

const DRAG_THRESHOLD_PX = 5;
const SET_PARAM_INTERVAL_MS = 33; // ~30 Hz

const expanded = new Set<string>(); // node ids whose knobs are shown (kept across re-renders)
const disposers = new WeakMap<HTMLElement, () => void>();

const isChainNode = (n: RigNode) => n.type !== 'Input' && n.type !== 'Output';
const baseName = (path: string) => path.split(/[\\/]/).pop()!.replace(/\.[^.]+$/, '');
const assetKind = (type: string) => (type === 'NamAmp' ? 'nam' : type === 'CabIR' ? 'ir' : '');
const showError = (e: unknown) => toast((e as Error).message ?? String(e));

function plugin(n: RigNode) {
  return n.plugin as { format?: string; uid?: string; name?: string; vendor?: string } | undefined;
}
function assetFile(n: RigNode): string {
  return (n.asset as { file?: string } | undefined)?.file ?? '';
}

export function nodeTitle(n: RigNode): string {
  if (n.type === 'Plugin') return plugin(n)?.name || 'Plugin';
  return BUILTINS.find((b) => b.type === n.type)?.label ?? n.type;
}

/** "footswitch:L" -> "L", "midi:<device>:cc64" -> "MIDI cc64". */
export function controlLabel(control: string): string {
  if (control.startsWith('footswitch:')) return control.slice('footswitch:'.length);
  if (control.startsWith('midi:')) return `MIDI ${control.slice(control.lastIndexOf(':') + 1)}`;
  return control;
}

function cardKind(type: string): string {
  return type === 'NamAmp' ? 'amp' : type === 'CabIR' ? 'cab' : type === 'Plugin' ? 'plugin' : 'pedal';
}

/** Everything that needs the cards rebuilt. Params and bypass are updated in place (knobs may be mid-drag). */
function structureKey(s: AppState): string {
  if (!s.connected) return 'connecting';
  if (!s.rig) return 'none';
  return JSON.stringify({
    id: s.rig.id,
    nodes: s.rig.nodes.filter(isChainNode).map((n) => [n.id, n.type, assetFile(n), plugin(n)?.name, n.art]),
    mappings: s.rig.mappings ?? [],
    placeholders: s.report.placeholderNodeIds,
    expanded: [...expanded],
  });
}

export function renderEdit(root: HTMLElement): void {
  disposers.get(root)?.();

  const board = el('div', { class: 'pedalboard', role: 'list', 'aria-label': 'Signal chain' });
  const overlayHost = el('div', { class: 'edit-overlay-host' });
  root.replaceChildren(el('div', { class: 'edit-view' }, board, overlayHost));

  let key = '';
  let knobs = new Map<string, Knob>(); // `${nodeId}/${param}`
  let toggles = new Map<string, HTMLButtonElement>();
  let cards = new Map<string, HTMLElement>(); // node id -> card
  let dragActive = false;
  let learningNode = ''; // the node a learn was armed for (for the overlay text)

  function render(s: AppState) {
    const k = structureKey(s);
    if (k !== key && !dragActive) {
      key = k;
      rebuild(s);
    }
    updateLive(s);
    renderLearnOverlay(s);
  }
  const rerender = () => {
    key = '';
    render(store.get());
  };

  function rebuild(s: AppState) {
    knobs = new Map();
    toggles = new Map();
    cards = new Map();
    if (!s.connected) {
      board.replaceChildren(el('p', { class: 'muted center' }, 'Connecting to the engine…'));
      return;
    }
    if (!s.rig) {
      board.replaceChildren(el('p', { class: 'muted center' }, 'No rig loaded. Open the library to load or create one.'));
      return;
    }
    const chain = s.rig.nodes.filter(isChainNode);
    const add = el('button', { class: 'card card-add', 'aria-label': 'Add to the chain', title: 'Add a pedal, amp, cab or plugin' }, '+');
    add.addEventListener('click', () => {
      const r = add.getBoundingClientRect();
      openAddMenu(chain.length, r.left, r.bottom + 4);
    });
    board.replaceChildren(...chain.map((n, i) => buildCard(n, i, chain, s)), add);
  }

  function updateLive(s: AppState) {
    for (const n of s.rig?.nodes ?? []) {
      const card = cards.get(n.id);
      if (!card) continue;
      card.classList.toggle('off', n.bypass);
      card.querySelector('.led')?.setAttribute('aria-pressed', String(!n.bypass));
      for (const spec of PARAMS[n.type] ?? []) {
        const v = n.params?.[spec.name];
        if (typeof v !== 'number') continue;
        knobs.get(`${n.id}/${spec.name}`)?.setValue(v);
        toggles.get(`${n.id}/${spec.name}`)?.setAttribute('aria-pressed', String(v >= 0.5));
      }
    }
  }

  function buildCard(n: RigNode, index: number, chain: RigNode[], s: AppState): HTMLElement {
    const placeholder = s.report.placeholderNodeIds.includes(n.id);
    const title = nodeTitle(n);
    const isOpen = expanded.has(n.id) && !placeholder;
    const card = el('div', {
      class: `card card-${cardKind(n.type)}`,
      role: 'listitem',
      tabindex: '0',
      'data-node-id': n.id,
      'data-type': n.type,
      'aria-label': title,
      'aria-expanded': String(isOpen),
      title: 'Click for knobs, drag to move, right-click for more',
    });
    const archetype = (n.art as { archetype?: string } | undefined)?.archetype;
    if (archetype) card.dataset.archetype = archetype;
    card.classList.toggle('off', n.bypass);
    card.classList.toggle('missing', placeholder);
    card.classList.toggle('expanded', isOpen);

    const led = el('button', { class: 'led', 'aria-label': `${title} on`, 'aria-pressed': String(!n.bypass), title: 'On / off' });
    led.disabled = placeholder;
    led.addEventListener('click', () => call('toggleBypass', { id: n.id }).catch(showError));
    const menuButton = el('button', { class: 'icon-button card-menu-button', 'aria-label': `${title} menu`, 'aria-haspopup': 'menu', title: 'More' }, '⋯');
    menuButton.addEventListener('click', () => {
      const r = menuButton.getBoundingClientRect();
      openNodeMenu(n, placeholder, r.left, r.bottom + 4);
    });

    const parts: Node[] = [el('div', { class: 'card-header' }, led, menuButton), el('div', { class: 'card-title' }, title)];

    if (placeholder) {
      const original = plugin(n)?.name || (assetFile(n) ? baseName(assetFile(n)) : n.type);
      parts.push(el('span', { class: 'badge-missing' }, 'missing'), el('div', { class: 'card-sub', title: assetFile(n) || original }, original));
    } else if (assetKind(n.type)) {
      const file = assetFile(n);
      const pick = el(
        'button',
        { class: 'asset-button', title: file || 'Choose a file' },
        file ? baseName(file) : n.type === 'NamAmp' ? 'Choose model…' : 'Choose IR…',
      );
      pick.addEventListener('click', () => void pickAsset(n.id, assetKind(n.type) as 'nam' | 'ir'));
      parts.push(pick);
    } else if (n.type === 'Plugin' && plugin(n)?.vendor) {
      parts.push(el('div', { class: 'card-sub' }, plugin(n)!.vendor!));
    }

    if (isOpen) parts.push(buildControls(n));

    const labels = (s.rig?.mappings ?? []).filter((m: Mapping) => m.node === n.id).map((m) => controlLabel(m.control));
    if (labels.length > 0) parts.push(el('div', { class: 'fs-label', title: 'Footswitch' }, labels.join(' · ')));

    card.append(...parts);

    card.addEventListener('contextmenu', (e) => {
      e.preventDefault();
      openNodeMenu(n, placeholder, e.clientX, e.clientY);
    });
    card.addEventListener('keydown', (e) => {
      if (e.target !== card) return;
      if (e.key === 'Enter' || e.key === ' ') {
        e.preventDefault();
        toggleExpanded(n.id);
      } else if (e.altKey && (e.key === 'ArrowLeft' || e.key === 'ArrowRight')) {
        e.preventDefault();
        const to = index + (e.key === 'ArrowLeft' ? -1 : 1);
        if (to >= 0 && to < chain.length) call('moveNode', { id: n.id, index: to }).catch(showError);
      } else if (e.key === 'ContextMenu') {
        e.preventDefault();
        const r = card.getBoundingClientRect();
        openNodeMenu(n, placeholder, r.left, r.top + 24);
      }
    });
    card.addEventListener('pointerdown', (e) => startPointer(e, card, n.id, index));
    cards.set(n.id, card);
    return card;
  }

  function buildControls(n: RigNode): HTMLElement {
    const box = el('div', { class: 'card-controls' });
    if (n.type === 'Plugin') {
      const open = el('button', { class: 'button small' }, 'Open editor');
      open.addEventListener('click', () => call('openPluginEditor', { id: n.id }).catch(showError));
      box.append(open);
      return box;
    }
    for (const spec of PARAMS[n.type] ?? []) {
      const current = typeof n.params?.[spec.name] === 'number' ? n.params[spec.name] : spec.defaultValue;
      const send = throttle((value: number) => call('setParam', { id: n.id, name: spec.name, value }).catch(showError), SET_PARAM_INTERVAL_MS);
      if (spec.toggle) {
        const t = el('button', { class: 'toggle', 'aria-pressed': String(current >= 0.5), title: spec.label }, spec.label);
        t.addEventListener('click', () => {
          const on = t.getAttribute('aria-pressed') !== 'true';
          t.setAttribute('aria-pressed', String(on));
          send(on ? 1 : 0);
        });
        toggles.set(`${n.id}/${spec.name}`, t);
        box.append(t);
      } else {
        const knob = createKnob({ ...spec, value: current, onChange: send });
        knobs.set(`${n.id}/${spec.name}`, knob);
        box.append(knob.el);
      }
    }
    return box;
  }

  function toggleExpanded(id: string) {
    if (expanded.has(id)) expanded.delete(id);
    else expanded.add(id);
    rerender();
  }

  // ---- drag to reorder (pointer events: HTML5 drag and drop is unreliable in WKWebView) ----
  function startPointer(e: PointerEvent, card: HTMLElement, id: string, from: number) {
    if (e.button !== 0) return;
    if ((e.target as Element).closest('button, .knob, input, select, a')) return;
    const startX = e.clientX;
    const startY = e.clientY;
    let others: { el: HTMLElement; mid: number }[] = [];
    let target = from;

    const dropIndex = (x: number) => others.filter((o) => o.mid < x).length;
    const mark = () => {
      others.forEach((o, i) => {
        o.el.classList.toggle('drop-before', i === target && target !== from);
        o.el.classList.toggle('drop-after', i === others.length - 1 && target === others.length && target !== from);
      });
    };
    const move = (m: PointerEvent) => {
      const dx = m.clientX - startX;
      if (!dragActive) {
        if (Math.abs(dx) < DRAG_THRESHOLD_PX && Math.abs(m.clientY - startY) < DRAG_THRESHOLD_PX) return;
        dragActive = true;
        card.classList.add('dragging');
        others = [...cards.values()]
          .filter((c) => c !== card)
          .map((c) => {
            const r = c.getBoundingClientRect();
            return { el: c, mid: r.left + r.width / 2 };
          });
      }
      card.style.transform = `translateX(${dx}px)`;
      target = dropIndex(m.clientX);
      mark();
    };
    const up = (u: PointerEvent) => {
      window.removeEventListener('pointermove', move);
      window.removeEventListener('pointerup', up);
      window.removeEventListener('pointercancel', cancel);
      if (!dragActive) {
        toggleExpanded(id);
        return;
      }
      target = dropIndex(u.clientX);
      endDrag();
      if (target !== from) call('moveNode', { id, index: target }).catch(showError);
    };
    const cancel = () => {
      window.removeEventListener('pointermove', move);
      window.removeEventListener('pointerup', up);
      window.removeEventListener('pointercancel', cancel);
      if (dragActive) endDrag();
    };
    const endDrag = () => {
      dragActive = false;
      card.classList.remove('dragging');
      card.style.transform = '';
      others.forEach((o) => o.el.classList.remove('drop-before', 'drop-after'));
      render(store.get()); // a rebuild may have waited for the drag to end
    };
    window.addEventListener('pointermove', move);
    window.addEventListener('pointerup', up);
    window.addEventListener('pointercancel', cancel);
  }

  // ---- menus ----
  function openNodeMenu(n: RigNode, placeholder: boolean, x: number, y: number) {
    const items: MenuItem[] = [];
    if (!placeholder && assetKind(n.type))
      items.push({ label: n.type === 'NamAmp' ? 'Choose model…' : 'Choose IR…', run: () => void pickAsset(n.id, assetKind(n.type) as 'nam' | 'ir') });
    if (!placeholder && n.type === 'Plugin')
      items.push({ label: 'Open editor', run: () => call('openPluginEditor', { id: n.id }).catch(showError) });
    items.push(
      { label: 'Learn footswitch', run: () => void learn(n.id) },
      { label: 'Set art…', run: () => openArtDialog(n) },
      'separator',
      { label: 'Remove', danger: true, run: () => call('removeNode', { id: n.id }).catch(showError) },
    );
    openMenu(items, x, y, `${nodeTitle(n)} menu`);
  }

  function openAddMenu(index: number, x: number, y: number) {
    openMenu(
      [
        ...BUILTINS.map((b) => ({ label: b.label, run: () => void addBuiltin(b.type, index) })),
        'separator' as const,
        { label: 'Plugins…', run: () => openPluginPicker(index) },
      ],
      x,
      y,
      'Add to the chain',
    );
  }

  async function addBuiltin(type: string, index: number) {
    try {
      const { id } = await call<{ id: string }>('addNode', { type, index });
      const kind = assetKind(type);
      if (kind) await pickAsset(id, kind as 'nam' | 'ir');
    } catch (e) {
      showError(e);
    }
  }

  // ---- footswitch learn ----
  async function learn(id: string) {
    try {
      await call('learnMapping', { id });
      learningNode = id;
      setLearning(true);
    } catch (e) {
      showError(e);
    }
  }
  function cancelLearn() {
    setLearning(false);
    call('cancelLearn').catch(showError);
  }

  function renderLearnOverlay(s: AppState) {
    if (!s.learning) {
      overlayHost.replaceChildren();
      return;
    }
    if (overlayHost.firstChild) return;
    const node = s.rig?.nodes.find((n) => n.id === learningNode);
    const cancel = el('button', { class: 'button' }, 'Cancel');
    cancel.addEventListener('click', cancelLearn);
    overlayHost.replaceChildren(
      el(
        'div',
        { class: 'learn-overlay', role: 'dialog', 'aria-modal': 'true', 'aria-label': 'Learn footswitch' },
        el(
          'div',
          { class: 'learn-box' },
          el('p', { class: 'learn-title', 'aria-live': 'assertive' }, 'Press a footswitch…'),
          el('p', { class: 'muted' }, node ? `It will switch “${nodeTitle(node)}” on and off. Esc cancels.` : 'Esc cancels.'),
          cancel,
        ),
      ),
    );
    cancel.focus();
  }

  const onKey = (e: KeyboardEvent) => {
    if (e.key === 'Escape' && store.get().learning) cancelLearn();
  };
  document.addEventListener('keydown', onKey);
  const unsubscribe = store.subscribe(render);
  disposers.set(root, () => {
    unsubscribe();
    document.removeEventListener('keydown', onKey);
  });
  render(store.get());
}

// ---- pickers and dialogs (modal, shared by every card) ----

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

function openPluginPicker(index: number) {
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

function openArtDialog(n: RigNode) {
  const kind = n.type === 'NamAmp' ? 'amp' : n.type === 'CabIR' ? 'cab' : 'pedal';
  const current = (n.art as { archetype?: string } | undefined)?.archetype ?? '';
  const modal = openModal(`Art for ${nodeTitle(n)}`, undefined, 'modal-small');
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
