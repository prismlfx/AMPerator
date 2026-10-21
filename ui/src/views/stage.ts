// Stage view (spec §6, mockup C): the screen the app opens into. Band, rig name and tags, the amp → cab line with art
// thumbnails, three big footswitch tiles (mapped node, lit while it is on, flashing on every press), previous /
// next rig, the input meter, the tuner and ✎ Edit. Keys (when no text field has focus): ← / → rig, T tuner,
// E edit, Esc closes the tuner.

import { call, on } from '../bridge';
import { el, toast } from '../dialog';
import { ARCHETYPES, resolveArt, nodeArchetype, type NodeView } from '../art/archetypes';
import { confirmDiscardIfDirty, setTunerOn, setView, store, type AppState, type RigNode } from '../state';
import { isTyping } from '../keys';
import { nodeTitle } from './edit';

export const FOOTSWITCHES = ['footswitch:L', 'footswitch:M', 'footswitch:R'] as const;

const METER_FLOOR_DB = -60;
const PEAK_HOLD_MS = 1000;
const CLIP_DB = -1;
const FLASH_MS = 250;
const IN_TUNE_CENTS = 3;

const showError = (e: unknown) => toast((e as Error).message ?? String(e));
const baseName = (path: string) => path.split(/[\\/]/).pop()!.replace(/\.[^.]+$/, '');

/** Input meter model: level and held peak as 0..100 % of the -60..0 dBFS scale; `clip` stays on for ~1 s after
    the input went above -1 dBFS. */
export class PeakMeter {
  private peak = 0;
  private peakAt = -Infinity;
  private clipAt = -Infinity;

  update(db: number, nowMs: number): { level: number; peak: number; clip: boolean } {
    const level = Math.min(100, Math.max(0, ((db - METER_FLOOR_DB) * 100) / -METER_FLOOR_DB));
    if (level >= this.peak || nowMs - this.peakAt > PEAK_HOLD_MS) {
      this.peak = level;
      this.peakAt = nowMs;
    }
    if (db > CLIP_DB) this.clipAt = nowMs;
    return { level, peak: this.peak, clip: nowMs - this.clipAt <= PEAK_HOLD_MS };
  }
}

/** The node a control toggles: the rig's mapping, else the global one (rig mappings override global ones). */
export function mappedNode(s: AppState, control: string): RigNode | undefined {
  const m =
    (s.rig?.mappings ?? []).find((x) => x.control === control) ?? (s.settings.globalMappings ?? []).find((x) => x.control === control);
  return m ? s.rig?.nodes.find((n) => n.id === m.node) : undefined;
}

/** The amp and cab shown in the amp → cab line: the first NamAmp / CabIR, else a plugin drawn as one. */
function gearNodes(rig: AppState['rig']): RigNode[] {
  const nodes = (rig?.nodes ?? []) as NodeView[];
  const isKind = (kind: readonly string[], type: string) => (n: NodeView) =>
    n.type === type || (n.type === 'Plugin' && kind.includes(nodeArchetype(n)));
  const amp = nodes.find(isKind(ARCHETYPES.amp, 'NamAmp'));
  const cab = nodes.find(isKind(ARCHETYPES.cab, 'CabIR'));
  return [amp, cab].filter((n): n is NodeView => !!n);
}

function gearName(n: RigNode): string {
  const file = (n.asset as { file?: string } | undefined)?.file;
  return file ? baseName(file) : nodeTitle(n);
}

/** Renders the stage view into `root`; returns a function that removes its listeners. */
export function renderStage(root: HTMLElement): () => void {
  // ---- toolbar: tuner, input meter, edit ----
  const tunerButton = el('button', { class: 'button stage-tuner-button', 'aria-pressed': 'false', title: 'Tuner (T)' }, '♩ Tuner');
  tunerButton.addEventListener('click', () => void setTuner(!store.get().tunerOn));
  const meterFill = el('div', { class: 'meter-fill' });
  const meterPeak = el('div', { class: 'meter-peak' });
  const meterClip = el('span', { class: 'meter-clip', title: 'Clipping: turn the input down' }, 'CLIP');
  const meter = el(
    'div',
    { class: 'stage-meter', role: 'meter', 'aria-label': 'Input level', 'aria-valuemin': '-60', 'aria-valuemax': '0', 'aria-valuenow': '-60' },
    el('span', { class: 'stage-meter-label' }, 'IN'),
    el('div', { class: 'meter-track' }, meterFill, meterPeak),
    meterClip,
  );
  const editButton = el('button', { class: 'button stage-edit', title: 'Edit the rig (E)' }, '✎ Edit');
  editButton.addEventListener('click', () => setView('edit'));

  // ---- rig name, amp → cab ----
  const body = el('div', { class: 'stage-body' });

  // ---- footswitch tiles ----
  const flashTimers = new Map<string, ReturnType<typeof setTimeout>>();
  const tiles = FOOTSWITCHES.map((control) => {
    const name = el('span', { class: 'fs-tile-name' }, '—');
    const t = el(
      'button',
      { class: 'fs-tile', 'data-control': control, 'aria-pressed': 'false' },
      el('span', { class: 'fs-tile-key' }, control.slice('footswitch:'.length)),
      name,
    );
    t.addEventListener('click', () => {
      const node = mappedNode(store.get(), control);
      if (node && !store.get().report.placeholderNodeIds.includes(node.id)) call('toggleBypass', { id: node.id }).catch(showError);
    });
    return { control, el: t, name };
  });
  const tileRow = el('div', { class: 'fs-tiles', role: 'group', 'aria-label': 'Footswitches' }, ...tiles.map((t) => t.el));

  // ---- previous / next rig ----
  const prev = el('button', { class: 'button stage-prev', title: 'Previous rig (←)' }, '◀ Prev rig');
  const next = el('button', { class: 'button stage-next', title: 'Next rig (→)' }, 'Next rig ▶');
  prev.addEventListener('click', () => void step(-1));
  next.addEventListener('click', () => void step(1));

  // ---- tuner overlay ----
  const tunerNote = el('span', { class: 'tuner-note' }, '—');
  const tunerOctave = el('span', { class: 'tuner-octave' });
  const tunerNeedle = el('div', { class: 'tuner-needle' });
  const tunerCents = el('div', { class: 'tuner-cents' });
  const tunerHz = el('div', { class: 'tuner-hz' });
  const tunerClose = el('button', { class: 'button', title: 'Close the tuner (Esc)' }, 'Close');
  tunerClose.addEventListener('click', () => void setTuner(false));
  const tunerOverlay = el(
    'div',
    { class: 'tuner-overlay', role: 'dialog', 'aria-label': 'Tuner' },
    el(
      'div',
      { class: 'tuner-box' },
      el('div', { class: 'tuner-reading', 'aria-live': 'polite' }, tunerNote, tunerOctave),
      el(
        'div',
        { class: 'tuner-scale', 'aria-hidden': 'true' },
        el('div', { class: 'tuner-zone', style: `left:${50 - IN_TUNE_CENTS}%;width:${2 * IN_TUNE_CENTS}%` }),
        ...[-50, -25, 0, 25, 50].map((c) => el('div', { class: 'tuner-tick', style: `left:${50 + c}%` })),
        tunerNeedle,
      ),
      el('div', { class: 'tuner-labels', 'aria-hidden': 'true' }, el('span', {}, '−50'), el('span', {}, '0'), el('span', {}, '+50')),
      tunerCents,
      tunerHz,
      tunerClose,
    ),
  );
  tunerOverlay.hidden = true;

  root.replaceChildren(
    el(
      'div',
      { class: 'stage-view' },
      el('div', { class: 'stage-toolbar' }, tunerButton, meter, el('div', { class: 'spacer' }), editButton),
      body,
      tileRow,
      el('div', { class: 'stage-nav' }, prev, next),
      tunerOverlay,
    ),
  );

  // ---- behaviour ----
  async function step(direction: 1 | -1) {
    if (!(await confirmDiscardIfDirty())) return;
    try {
      await call(direction > 0 ? 'nextRig' : 'prevRig');
    } catch (e) {
      showError(e);
    }
  }

  let tunerRequest = 0;
  let tunerPending: boolean | null = null; // the state last requested, until its reply
  async function setTuner(onOff: boolean) {
    if (tunerPending === onOff) return;
    const request = ++tunerRequest;
    tunerPending = onOff;
    try {
      const r = await call<{ on: boolean }>('setTuner', { on: onOff });
      if (request === tunerRequest) setTunerOn(!!r.on);
    } catch (e) {
      showError(e);
    } finally {
      if (request === tunerRequest) tunerPending = null;
    }
  }

  function showTuner(d: { active: boolean; hz: number; note: string; octave: number; cents: number } | null) {
    const active = !!d && d.active && !!d.note;
    const cents = active ? Math.max(-50, Math.min(50, d!.cents)) : 0;
    tunerNote.textContent = active ? d!.note : '—';
    tunerNote.classList.toggle('idle', !active);
    tunerOctave.textContent = active ? String(d!.octave) : '';
    tunerHz.textContent = active ? `${d!.hz.toFixed(1)} Hz` : '';
    tunerCents.textContent = active ? `${cents > 0 ? '+' : cents < 0 ? '−' : ''}${Math.abs(Math.round(cents))} ¢` : '';
    tunerNeedle.style.left = `${50 + cents}%`;
    tunerNeedle.classList.toggle('idle', !active);
    tunerOverlay.classList.toggle('in-tune', active && Math.abs(d!.cents) <= IN_TUNE_CENTS);
  }

  const peakMeter = new PeakMeter();
  function showMeter(inDb: number) {
    const m = peakMeter.update(inDb, performance.now());
    meterFill.style.width = `${m.level}%`;
    meterPeak.style.left = `${m.peak}%`;
    meterClip.classList.toggle('on', m.clip);
    meter.setAttribute('aria-valuenow', String(Math.round(Math.max(METER_FLOOR_DB, Math.min(0, inDb)))));
  }

  function flash(control: string) {
    const tile = tiles.find((t) => t.control === control);
    if (!tile) return;
    clearTimeout(flashTimers.get(control));
    tile.el.classList.remove('flash');
    void tile.el.offsetWidth; // restart the animation on quick repeats
    tile.el.classList.add('flash');
    flashTimers.set(
      control,
      setTimeout(() => tile.el.classList.remove('flash'), FLASH_MS),
    );
  }

  let bodyKey = '';
  function renderBody(s: AppState) {
    const gear = gearNodes(s.rig) as NodeView[];
    const tags = s.rig?.tags ?? [];
    const key = JSON.stringify(
      !s.connected
        ? 'connecting'
        : !s.rig
          ? 'none'
          : [s.rig.name, s.rig.band, tags, gear.map((n) => [n.id, gearName(n), resolveArt(n)])],
    );
    if (key === bodyKey) return;
    bodyKey = key;
    if (!s.connected) {
      body.replaceChildren(el('p', { class: 'muted stage-message' }, 'Connecting to the engine…'));
      return;
    }
    if (!s.rig) {
      body.replaceChildren(el('p', { class: 'muted stage-message' }, 'No rig loaded. Open the library (☰) to load or create one.'));
      return;
    }
    const line: Node[] = [];
    gear.forEach((n, i) => {
      if (i > 0) line.push(el('span', { class: 'stage-arrow', 'aria-hidden': 'true' }, '→'));
      line.push(
        el(
          'div',
          { class: 'stage-gear', 'data-node-id': n.id },
          el('img', { class: 'stage-gear-art', src: resolveArt(n), alt: '', draggable: 'false' }),
          el('span', { class: 'stage-gear-name' }, gearName(n)),
        ),
      );
    });
    body.replaceChildren(
      el('div', { class: 'stage-band' }, s.rig.band),
      el('h1', { class: 'stage-rig-name' }, s.rig.name),
      ...(tags.length > 0 ? [el('div', { class: 'stage-tags muted', 'aria-label': 'Tags' }, tags.join(' · '))] : []),
      el('div', { class: 'stage-line', 'aria-label': 'Amp and cab' }, ...line),
    );
  }

  function renderLive(s: AppState) {
    const placeholders = s.report.placeholderNodeIds;
    for (const g of body.querySelectorAll<HTMLElement>('.stage-gear')) {
      const n = s.rig?.nodes.find((x) => x.id === g.dataset.nodeId);
      g.classList.toggle('off', !!n?.bypass);
      g.classList.toggle('missing', !!n && placeholders.includes(n.id));
    }
    for (const t of tiles) {
      const node = s.rig ? mappedNode(s, t.control) : undefined;
      const usable = !!node && !placeholders.includes(node.id);
      const lit = usable && !node!.bypass;
      t.name.textContent = node ? nodeTitle(node) : '—';
      t.el.classList.toggle('lit', lit);
      t.el.classList.toggle('unmapped', !node);
      t.el.setAttribute('aria-pressed', String(lit));
      t.el.setAttribute('aria-disabled', String(!usable));
      t.el.setAttribute('aria-label', `Footswitch ${t.control.slice(-1)}: ${node ? nodeTitle(node) : 'not mapped'}`);
      t.el.title = node ? `${nodeTitle(node)} — click to switch on/off` : 'Not mapped: right-click a node in Edit → Learn footswitch';
    }
    for (const b of [prev, next]) b.disabled = !s.connected;
    tunerButton.setAttribute('aria-pressed', String(s.tunerOn));
    const wasHidden = tunerOverlay.hidden;
    tunerOverlay.hidden = !s.tunerOn;
    if (wasHidden && s.tunerOn) showTuner(null);
  }

  function render(s: AppState) {
    // The tuner belongs to the stage: leaving the stage switches it off.
    if (s.view !== 'stage' && s.tunerOn) void setTuner(false);
    renderBody(s);
    renderLive(s);
  }

  const onKey = (e: KeyboardEvent) => {
    const s = store.get();
    if (s.view !== 'stage' || e.metaKey || e.ctrlKey || e.altKey || e.repeat || e.defaultPrevented) return;
    if (isTyping(e.target) || s.learning || document.querySelector('.modal-backdrop')) return;
    const k = e.key.length === 1 ? e.key.toLowerCase() : e.key;
    if (k === 'ArrowLeft' || k === 'ArrowRight') void step(k === 'ArrowLeft' ? -1 : 1);
    else if (k === 't') void setTuner(!s.tunerOn);
    else if (k === 'e') setView('edit');
    else if (k === 'Escape' && s.tunerOn) void setTuner(false);
    else return;
    e.preventDefault();
  };
  document.addEventListener('keydown', onKey);

  const offs = [
    store.subscribe(render),
    on('meter', (d) => showMeter(typeof d?.inDb === 'number' ? d.inDb : -100)),
    on('tuner', (d) => showTuner(d ?? null)),
    on('controlEvent', (d) => {
      if (d?.pressed) flash(d.control);
    }),
  ];
  render(store.get());

  return () => {
    for (const off of offs) off();
    for (const t of flashTimers.values()) clearTimeout(t);
    document.removeEventListener('keydown', onKey);
  };
}
