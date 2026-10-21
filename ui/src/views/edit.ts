// Edit view (pedalboard): the rig's nodes as pedal / amp / cab cards in signal order (Input and Output are not
// shown). Click a card to show its knobs, drag to reorder, + to add, right-click or ⋯ for the card menu
// (footswitch learn / clear, art, plugin editor, remove). Placeholder (missing) nodes are drawn bypassed, without knobs;
// a missing model or IR offers Locate… (pick the file from the library). Above the board: the rig's tags (style,
// decade, band, song — setTags). Below the board: the master level (the Output node's `level`).

import { call } from '../bridge';
import { createKnob, throttle, type Knob } from '../components/knob';
import { el, openMenu, toast, type MenuItem } from '../dialog';
import { openArtDialog, openPluginPicker, pickAsset } from '../components/pickers';
import { createTagEditor } from '../components/tagEditor';
import { resolveArt, type NodeView } from '../art/archetypes';
import { setLearning, store, type AppState, type Mapping, type RigNode } from '../state';

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
  Output: [{ name: 'level', label: 'Master', min: -60, max: 12, defaultValue: 0, unit: 'dB' }],
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
  const master = el('div', { class: 'master-level', 'aria-label': 'Master level' });
  const tagEditor = createTagEditor({
    tags: [],
    label: 'Rig tags',
    onChange: (tags) => void saveTags(tags),
    suggestions: async () => (await call<{ tags: { tag: string }[] }>('listTags')).tags.map((t) => t.tag),
  });
  const tagRow = el('div', { class: 'edit-tags' }, el('span', { class: 'edit-tags-label' }, 'Tags'), tagEditor.el);
  root.replaceChildren(el('div', { class: 'edit-view' }, tagRow, board, master));

  let key = '';
  let knobs = new Map<string, Knob>(); // `${nodeId}/${param}`
  let toggles = new Map<string, HTMLButtonElement>();
  let cards = new Map<string, HTMLElement>(); // node id -> card
  let dragActive = false;
  // Bumped by every rebuild: knob/toggle senders of discarded cards check it and drop their sends.
  let generation = 0;

  let tagsKey = '';
  function renderTags(s: AppState) {
    tagRow.hidden = !s.connected || !s.rig;
    const k = JSON.stringify([s.rig?.id, s.rig?.tags ?? []]);
    if (k === tagsKey) return;
    tagsKey = k;
    tagEditor.setTags(s.rig?.tags ?? []);
  }

  /** Every add / remove sends the complete list; the engine answers with tagsChanged (the normalized list). */
  async function saveTags(tags: string[]) {
    try {
      await call('setTags', { tags });
    } catch (e) {
      showError(e);
      tagsKey = ''; // show the rig's tags again
      renderTags(store.get());
    }
  }

  function render(s: AppState) {
    renderTags(s);
    const k = structureKey(s);
    if (k !== key && !dragActive) {
      key = k;
      rebuild(s);
    }
    updateLive(s);
  }
  const rerender = () => {
    key = '';
    render(store.get());
  };

  function rebuild(s: AppState) {
    generation++;
    for (const k of knobs.values()) k.dispose(); // ends a knob drag in progress on the old cards
    knobs = new Map();
    toggles = new Map();
    cards = new Map();
    master.replaceChildren();
    if (!s.connected) {
      board.replaceChildren(el('p', { class: 'muted center' }, 'Connecting to the engine…'));
      return;
    }
    if (!s.rig) {
      board.replaceChildren(el('p', { class: 'muted center' }, 'No rig loaded. Open the library to load or create one.'));
      return;
    }
    const output = s.rig.nodes.find((n) => n.type === 'Output');
    if (output) master.append(el('span', { class: 'master-label' }, 'Output'), buildKnobs(output, PARAMS.Output));
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
      if (card) {
        card.classList.toggle('off', n.bypass);
        card.querySelector('.led')?.setAttribute('aria-pressed', String(!n.bypass));
      }
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

    const art = el('img', { class: 'card-art', src: resolveArt(n as NodeView), alt: '', draggable: 'false' });
    const parts: Node[] = [el('div', { class: 'card-header' }, led, menuButton), art, el('div', { class: 'card-title' }, title)];

    if (placeholder) {
      const original = plugin(n)?.name || (assetFile(n) ? baseName(assetFile(n)) : n.type);
      parts.push(el('span', { class: 'badge-missing' }, 'missing'), el('div', { class: 'card-sub', title: assetFile(n) || original }, original));
      if (assetKind(n.type)) {
        const locate = el('button', { class: 'asset-button', title: 'Find the file in the library' }, 'Locate…');
        locate.addEventListener('click', () => void pickAsset(n.id, assetKind(n.type) as 'nam' | 'ir'));
        parts.push(locate);
      }
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
    if (n.type === 'Plugin') {
      const box = el('div', { class: 'card-controls' });
      const open = el('button', { class: 'button small' }, 'Open editor');
      open.addEventListener('click', () => call('openPluginEditor', { id: n.id }).catch(showError));
      box.append(open);
      return box;
    }
    return buildKnobs(n, PARAMS[n.type] ?? []);
  }

  function buildKnobs(n: RigNode, specs: ParamSpec[]): HTMLElement {
    const box = el('div', { class: 'card-controls' });
    for (const spec of specs) {
      const current = typeof n.params?.[spec.name] === 'number' ? n.params[spec.name] : spec.defaultValue;
      const gen = generation;
      // Dropped once the cards were rebuilt (another rig, a structural edit): the node may no longer exist. A reply
      // that fails after a rebuild is not shown either.
      const send = throttle((value: number) => {
        if (gen !== generation) return;
        call('setParam', { id: n.id, name: spec.name, value }).catch((e) => {
          if (gen === generation) showError(e);
        });
      }, SET_PARAM_INTERVAL_MS);
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
    if (placeholder && assetKind(n.type))
      items.push({ label: 'Locate…', run: () => void pickAsset(n.id, assetKind(n.type) as 'nam' | 'ir') });
    if (!placeholder && n.type === 'Plugin')
      items.push({ label: 'Open editor', run: () => call('openPluginEditor', { id: n.id }).catch(showError) });
    const mapped = (store.get().rig?.mappings ?? []).filter((m) => m.node === n.id);
    items.push(
      { label: 'Learn footswitch', run: () => void learn(n.id) },
      ...mapped.map((m) => ({
        label: `Clear footswitch ${controlLabel(m.control)}`,
        run: () => void call('clearMapping', { control: m.control }).catch(showError),
      })),
      { label: 'Set art…', run: () => openArtDialog(n, nodeTitle(n)) },
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
      setLearning(true, id);
    } catch (e) {
      showError(e);
    }
  }

  const unsubscribe = store.subscribe(render);
  disposers.set(root, () => {
    unsubscribe();
    tagEditor.dispose();
    generation++;
    for (const k of knobs.values()) k.dispose();
  });
  render(store.get());
}

function cancelLearn() {
  setLearning(false);
  call('cancelLearn').catch(showError);
}

/** "Press a footswitch…" overlay, shown in `host` (the app root, so in any view) while a learn is armed;
    Esc or Cancel calls cancelLearn, the `learned` event (or a rig load) ends it. */
export function mountLearnOverlay(host: HTMLElement): () => void {
  const layer = el('div', { class: 'learn-layer' });
  host.append(layer);
  const render = (s: AppState) => {
    if (!s.learning) {
      layer.replaceChildren();
      return;
    }
    if (layer.firstChild) return;
    const node = s.rig?.nodes.find((n) => n.id === s.learnNode);
    const cancel = el('button', { class: 'button' }, 'Cancel');
    cancel.addEventListener('click', cancelLearn);
    layer.replaceChildren(
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
  };
  const onKey = (e: KeyboardEvent) => {
    if (e.key === 'Escape' && store.get().learning) cancelLearn();
  };
  document.addEventListener('keydown', onKey);
  const unsubscribe = store.subscribe(render);
  render(store.get());
  return () => {
    unsubscribe();
    document.removeEventListener('keydown', onKey);
    layer.remove();
  };
}
