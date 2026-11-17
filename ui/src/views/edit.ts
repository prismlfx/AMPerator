// Edit view (pedalboard): the rig's nodes as pedal / amp / cab cards in signal order (Input and Output are not
// shown). Click a card to show its knobs, drag to reorder, + to add, right-click or ⋯ for the card menu
// (footswitch learn / clear, art, plugin editor, remove). Placeholder (missing) nodes are drawn bypassed, without knobs;
// a missing model or IR offers Locate… (pick the file from the library). Above the board: the rig's tags (style,
// decade, band, song — setTags). Below the board: the master level (the Output node's `level`).

import { call, openExternal } from '../bridge';
import { createKnob, throttle, type Knob } from '../components/knob';
import { el, openMenu, toast, type MenuItem } from '../dialog';
import { openArtDialog, openPluginPicker, pickAsset } from '../components/pickers';
import { createTagEditor } from '../components/tagEditor';
import { resolveArt, type NodeView } from '../art/archetypes';
import {
  connectTone3000,
  jobsFor,
  LONG_CALL_MS,
  markUrl,
  onJob,
  sourceInfo,
  type AssetSource,
} from '../online/tone3000';
import { openToneDetail } from './tone3000';
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

/** The TONE3000 model id of a node whose file came from TONE3000 (the rig records only provider and model id). */
function t3kModel(n: RigNode): string {
  const source = (n.asset as { source?: { provider?: string; id?: string } } | undefined)?.source;
  return source?.provider === 'tone3000' && source.id ? String(source.id) : '';
}
const downloading = (modelId: string) => jobsFor(modelId).some((j) => j.state !== 'done' && j.state !== 'failed');
const TONE3000_PREFIX = 'https://www.tone3000.com/';

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
function structureKey(s: AppState, sources: Map<string, AssetSource>): string {
  if (!s.connected) return 'connecting';
  if (!s.rig) return 'none';
  return JSON.stringify({
    id: s.rig.id,
    nodes: s.rig.nodes.filter(isChainNode).map((n) => [n.id, n.type, assetFile(n), plugin(n)?.name, n.art]),
    mappings: s.rig.mappings ?? [],
    placeholders: s.report.placeholderNodeIds,
    expanded: [...expanded],
    // TONE3000: what the cards show depends on the integration, the lookup and running downloads
    t3k: [
      s.tone3000.configured,
      ...s.rig.nodes.filter(isChainNode).map((n) => {
        const model = t3kModel(n);
        const info = sources.get(model);
        return model ? [model, info?.title, info?.creator?.username, info?.url, downloading(model)] : 0;
      }),
    ],
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

  // TONE3000 provenance of the library's files by model id (title, creator, link), looked up for the rig's nodes.
  let sources = new Map<string, AssetSource>();
  let lookupKey = '';
  let lookupRun = 0;
  let disposed = false;
  function refreshSources(s: AppState, force = false) {
    const wanted = (s.rig?.nodes ?? []).map(t3kModel).filter(Boolean).join(',');
    const k = `${s.rig?.id}/${wanted}`;
    if ((k === lookupKey && !force) || wanted === '') return;
    lookupKey = k;
    const run = ++lookupRun;
    void sourceInfo(force).then((found) => {
      if (disposed || run !== lookupRun) return;
      sources = found;
      render(store.get());
    });
  }

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
    const before = store.get().rig?.tags ?? [];
    const added = tags.filter((t) => !before.some((b) => b.toLowerCase() === t.toLowerCase()));
    try {
      await call('setTags', { tags });
    } catch (e) {
      showError(e);
      tagsKey = ''; // show the rig's tags again
      renderTags(store.get());
      // What was just typed goes back into the input to be fixed (unless something new was typed meanwhile).
      if (added.length > 0 && tagEditor.input.value === '') tagEditor.input.value = added.join(', ');
    }
  }

  function render(s: AppState) {
    renderTags(s);
    refreshSources(s);
    const k = structureKey(s, sources);
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
    const model = t3kModel(n);
    const mark = model ? [el('span', { class: 't3k-mark', title: 'From TONE3000' }, el('img', { src: markUrl(), alt: 'T3K', draggable: 'false' }))] : [];
    const parts: Node[] = [el('div', { class: 'card-header' }, led, ...mark, menuButton), art, el('div', { class: 'card-title' }, title)];

    if (placeholder) {
      const original = plugin(n)?.name || (assetFile(n) ? baseName(assetFile(n)) : n.type);
      parts.push(el('span', { class: 'badge-missing' }, 'missing'), el('div', { class: 'card-sub', title: assetFile(n) || original }, original));
      if (assetKind(n.type)) {
        const locate = el('button', { class: 'asset-button', title: 'Find the file in the library' }, 'Locate…');
        locate.addEventListener('click', () => void pickAsset(n.id, assetKind(n.type) as 'nam' | 'ir'));
        parts.push(locate);
      }
      if (model && s.tone3000.configured) {
        const busy = downloading(model);
        const download = el('button', { class: 'asset-button', title: 'Download the missing model from TONE3000' }, busy ? 'Downloading…' : 'Download from TONE3000');
        download.disabled = busy;
        download.addEventListener('click', () => void downloadFromTone3000(n.id));
        const replace = el('button', { class: 'asset-button', title: 'Pick a different model on TONE3000' }, 'Replace from TONE3000…');
        replace.addEventListener('click', () => void browseTone3000(n, true));
        parts.push(download, replace);
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
      if (!file && s.tone3000.configured) {
        const browse = el('button', { class: 'asset-button', title: 'Find one on TONE3000' }, 'Browse TONE3000');
        browse.addEventListener('click', () => void browseTone3000(n, false));
        parts.push(browse);
      }
    } else if (n.type === 'Plugin' && plugin(n)?.vendor) {
      parts.push(el('div', { class: 'card-sub' }, plugin(n)!.vendor!));
    }

    if (isOpen) {
      const credit = model ? buildCredit(sources.get(model)) : null;
      if (credit) parts.push(credit);
      parts.push(buildControls(n));
    }

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

  /** "<tone title> · <creator>" with a link to the tone's page, from the library's provenance record. */
  function buildCredit(info: AssetSource | undefined): HTMLElement | null {
    const text = [info?.title, info?.creator?.username].filter(Boolean).join(' · ');
    if (!info || !text) return null;
    const box = el('div', { class: 'card-credit' }, el('span', { class: 'card-credit-text', title: text }, text));
    const url = info.url ?? '';
    if (url.startsWith(TONE3000_PREFIX)) {
      const view = el('button', { class: 'asset-button', title: url }, 'View on TONE3000');
      view.addEventListener('click', () => void openExternal(url));
      box.append(view);
    }
    return box;
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
    const model = t3kModel(n);
    const t3k = store.get().tone3000.configured;
    if (t3k && !placeholder && assetKind(n.type)) items.push({ label: 'Browse TONE3000…', run: () => void browseTone3000(n, false) });
    if (t3k && placeholder && model && assetKind(n.type))
      items.push(
        { label: 'Download from TONE3000', run: () => void downloadFromTone3000(n.id) },
        { label: 'Replace from TONE3000…', run: () => void browseTone3000(n, true) },
      );
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

  // ---- TONE3000 ----
  const signedIn = async () => store.get().tone3000.connected || (await connectTone3000());

  /** The TONE3000 picker for node `n` (signing in first), then the chosen tone's window targeted at the node. With
      `replace` the engine opens the picker on the tone the node's file came from (its provenance index outlives the
      missing file), or the plain picker when it does not know it. */
  async function browseTone3000(n: RigNode, replace: boolean) {
    try {
      if (!(await signedIn())) return;
      const r = await call<{ toneId?: string; canceled?: boolean }>(
        'tone3000Browse',
        { format: assetKind(n.type), nodeId: n.id, ...(replace ? { replace: true } : {}) },
        LONG_CALL_MS,
      );
      if (!r.canceled && r.toneId) void openToneDetail(r.toneId, { nodeId: n.id });
    } catch (e) {
      showError(e);
    }
  }

  async function downloadFromTone3000(id: string) {
    try {
      if (await signedIn()) await call('tone3000Fetch', { nodeIds: [id] });
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
  const unsubscribeJobs = onJob((j) => {
    if (j.state === 'done') refreshSources(store.get(), true);
    render(store.get()); // Download buttons follow the jobs
  });
  disposers.set(root, () => {
    disposed = true;
    unsubscribe();
    unsubscribeJobs();
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
