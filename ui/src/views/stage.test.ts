import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { installFakeJuce, type FakeJuce } from '../test-support';

let juce: FakeJuce;
let state: typeof import('../state');
let stage: typeof import('./stage');
let art: typeof import('../art/archetypes');
let root: HTMLElement;
let unmount: (() => void) | null = null;

const emptyReport = { missingAssets: [], missingPlugins: [], placeholderNodeIds: [], resolvedByFilename: [] };

function makeRig(overrides: object = {}) {
  return {
    id: 'r1',
    name: 'Interstate',
    band: 'STP',
    nodes: [
      { id: 'in', type: 'Input', bypass: false, params: {}, art: { archetype: '', image: '' } },
      { id: 'drv', type: 'Plugin', bypass: true, params: {}, plugin: { name: 'Tube Drive', uid: 'u1' }, art: { archetype: '', image: '' } },
      { id: 'amp', type: 'NamAmp', bypass: false, params: {}, asset: { file: '/m/Mesa Rect.nam' }, art: { archetype: '', image: '' } },
      { id: 'cab', type: 'CabIR', bypass: false, params: {}, asset: { file: '/i/4x12 V30.wav' }, art: { archetype: '', image: '' } },
      { id: 'cho', type: 'Plugin', bypass: false, params: {}, plugin: { name: 'Chorus', uid: 'u2' }, art: { archetype: '', image: '' } },
      { id: 'out', type: 'Output', bypass: false, params: {}, art: { archetype: '', image: '' } },
    ],
    connections: [],
    mappings: [
      { control: 'footswitch:L', action: 'toggleBypass', node: 'drv' },
      { control: 'footswitch:M', action: 'toggleBypass', node: 'cho' },
    ],
    ...overrides,
  };
}

const flush = () => new Promise((r) => setTimeout(r, 0));
const tiles = () => [...root.querySelectorAll<HTMLButtonElement>('.fs-tile')];
const tile = (c: 'L' | 'M' | 'R') => root.querySelector<HTMLButtonElement>(`.fs-tile[data-control="footswitch:${c}"]`)!;
const lastCommand = () => juce.commands().at(-1);
const key = (k: string, target: EventTarget = document.body) =>
  target.dispatchEvent(new KeyboardEvent('keydown', { key: k, bubbles: true, cancelable: true }));

async function started(rig: object | null = makeRig(), extra: object = {}) {
  const p = state.initStore();
  const getState = juce.commands().find((c) => c.cmd === 'getState')!;
  juce.reply(getState.id, true, {
    protocol: 1, rig, report: emptyReport, dirty: false, saved: true, learning: false, startupErrors: [],
    settings: { globalMappings: [] }, deviceStatus: { status: 'Running', latencyMs: 8, xruns: 0 },
    controlStatus: { hid: 'NotFound', exclusive: false, midiDevices: [] }, tunerOn: false, favorites: [], recents: [], bands: ['STP'],
    ...extra,
  });
  await p;
}

beforeEach(async () => {
  vi.resetModules();
  vi.useRealTimers();
  document.body.innerHTML = '';
  juce = installFakeJuce();
  state = await import('../state');
  stage = await import('./stage');
  art = await import('../art/archetypes');
  root = document.createElement('div');
  document.body.append(root);
});

afterEach(() => {
  unmount?.();
  unmount = null;
  vi.useRealTimers();
  delete (window as any).__JUCE__;
});

describe('stage view', () => {
  it('the app opens into stage mode', async () => {
    await started();
    expect(state.store.get().view).toBe('stage');
  });

  it('shows band, rig name and the amp → cab line with art thumbnails', async () => {
    await started();
    unmount = stage.renderStage(root);
    expect(root.querySelector('.stage-band')!.textContent).toBe('STP');
    expect(root.querySelector('.stage-rig-name')!.textContent).toBe('Interstate');
    const gear = [...root.querySelectorAll<HTMLElement>('.stage-gear')];
    expect(gear.map((g) => g.querySelector('.stage-gear-name')!.textContent)).toEqual(['Mesa Rect', '4x12 V30']);
    const imgs = gear.map((g) => g.querySelector('img')!.getAttribute('src'));
    expect(imgs).toEqual([art.archetypeUrl('high-gain-rectifier'), art.archetypeUrl('cab-4x12')]);
    expect(root.querySelector('.stage-arrow')).not.toBeNull();
  });

  it('a user image replaces the amp thumbnail', async () => {
    const rig = makeRig();
    (rig.nodes[2] as any).art = { archetype: 'ac-chime', image: '/x/AMPerator/Art/me.png' };
    await started(rig);
    unmount = stage.renderStage(root);
    expect(root.querySelector('.stage-gear img')!.getAttribute('src')).toBe('art/me.png');
  });

  it('has three footswitch tiles labelled with the mapped node, lit when it is on', async () => {
    await started();
    unmount = stage.renderStage(root);
    expect(tiles()).toHaveLength(3);
    expect(tile('L').querySelector('.fs-tile-name')!.textContent).toBe('Tube Drive');
    expect(tile('M').querySelector('.fs-tile-name')!.textContent).toBe('Chorus');
    expect(tile('R').querySelector('.fs-tile-name')!.textContent).toBe('—');
    expect(tile('L').classList.contains('lit')).toBe(false); // drv is bypassed
    expect(tile('M').classList.contains('lit')).toBe(true);
    expect(tile('R').classList.contains('lit')).toBe(false);

    juce.fromBackend('amp', { v: 1, event: 'bypassChanged', data: { id: 'drv', bypass: false } });
    expect(tile('L').classList.contains('lit')).toBe(true);
    expect(tile('L').getAttribute('aria-pressed')).toBe('true');
  });

  it('uses global mappings unless the rig overrides the control', async () => {
    await started(makeRig(), {
      settings: {
        globalMappings: [
          { control: 'footswitch:R', action: 'toggleBypass', node: 'amp' },
          { control: 'footswitch:L', action: 'toggleBypass', node: 'cab' },
        ],
      },
    });
    unmount = stage.renderStage(root);
    expect(tile('R').querySelector('.fs-tile-name')!.textContent).toBe('NAM Amp');
    expect(tile('L').querySelector('.fs-tile-name')!.textContent).toBe('Tube Drive');
  });

  it('clicking a tile toggles its node; an unmapped tile does nothing', async () => {
    await started();
    unmount = stage.renderStage(root);
    tile('M').click();
    expect(lastCommand()).toMatchObject({ cmd: 'toggleBypass', args: { id: 'cho' } });
    const n = juce.commands().length;
    tile('R').click();
    expect(juce.commands()).toHaveLength(n);
  });

  it('flashes the tile of a pressed footswitch, mapped or not', async () => {
    vi.useFakeTimers();
    await started();
    unmount = stage.renderStage(root);
    juce.fromBackend('amp', { v: 1, event: 'controlEvent', data: { control: 'footswitch:R', pressed: true } });
    expect(tile('R').classList.contains('flash')).toBe(true);
    vi.advanceTimersByTime(400);
    expect(tile('R').classList.contains('flash')).toBe(false);
    juce.fromBackend('amp', { v: 1, event: 'controlEvent', data: { control: 'footswitch:R', pressed: false } });
    expect(tile('R').classList.contains('flash')).toBe(false);
  });

  it('next / previous rig go through the unsaved-changes check', async () => {
    await started();
    unmount = stage.renderStage(root);
    root.querySelector<HTMLButtonElement>('.stage-next')!.click();
    await flush();
    expect(lastCommand().cmd).toBe('nextRig');

    juce.fromBackend('amp', { v: 1, event: 'rigDirty', data: { dirty: true } });
    root.querySelector<HTMLButtonElement>('.stage-prev')!.click();
    await flush();
    expect(lastCommand().cmd).toBe('nextRig'); // waiting for the discard confirmation
    const ok = [...document.querySelectorAll<HTMLButtonElement>('.modal button')].find((b) => b.textContent === 'Discard changes')!;
    ok.click();
    await flush();
    expect(lastCommand().cmd).toBe('prevRig');
  });

  it('keyboard: arrows switch rigs, T toggles the tuner, E opens Edit; ignored while typing', async () => {
    await started();
    unmount = stage.renderStage(root);
    key('ArrowRight');
    await flush();
    expect(lastCommand().cmd).toBe('nextRig');
    key('ArrowLeft');
    await flush();
    expect(lastCommand().cmd).toBe('prevRig');
    key('t');
    expect(lastCommand()).toMatchObject({ cmd: 'setTuner', args: { on: true } });

    const input = document.createElement('input');
    document.body.append(input);
    input.focus();
    const n = juce.commands().length;
    key('ArrowRight', input);
    key('e', input);
    await flush();
    expect(juce.commands()).toHaveLength(n);
    expect(state.store.get().view).toBe('stage');
    input.blur();

    key('e');
    expect(state.store.get().view).toBe('edit');
    const m = juce.commands().length;
    key('ArrowRight'); // only in stage mode
    await flush();
    expect(juce.commands().filter((c) => c.cmd === 'nextRig')).toHaveLength(1);
    expect(juce.commands().length).toBeGreaterThanOrEqual(m);
  });

  it('the input meter follows meter events', async () => {
    await started();
    unmount = stage.renderStage(root);
    juce.fromBackend('amp', { v: 1, event: 'meter', data: { inDb: -30, outDb: -20 } });
    const fill = root.querySelector<HTMLElement>('.meter-fill')!;
    expect(fill.style.width).toBe('50%');
    expect(root.querySelector('.meter-clip')!.classList.contains('on')).toBe(false);
    juce.fromBackend('amp', { v: 1, event: 'meter', data: { inDb: -0.5, outDb: -20 } });
    expect(root.querySelector('.meter-clip')!.classList.contains('on')).toBe(true);
  });

  it('the tuner overlay shows note, octave, cents needle and Hz', async () => {
    await started();
    unmount = stage.renderStage(root);
    root.querySelector<HTMLButtonElement>('.stage-tuner-button')!.click();
    const cmd = lastCommand();
    expect(cmd).toMatchObject({ cmd: 'setTuner', args: { on: true } });
    juce.reply(cmd.id, true, { on: true });
    await flush();
    const overlay = root.querySelector<HTMLElement>('.tuner-overlay')!;
    expect(overlay.hidden).toBe(false);

    juce.fromBackend('amp', { v: 1, event: 'tuner', data: { active: true, hz: 110.2, note: 'A', octave: 2, cents: 25 } });
    expect(overlay.querySelector('.tuner-note')!.textContent).toBe('A');
    expect(overlay.querySelector('.tuner-octave')!.textContent).toBe('2');
    expect(overlay.querySelector('.tuner-hz')!.textContent).toBe('110.2 Hz');
    expect(overlay.querySelector<HTMLElement>('.tuner-needle')!.style.left).toBe('75%');
    expect(overlay.classList.contains('in-tune')).toBe(false);

    juce.fromBackend('amp', { v: 1, event: 'tuner', data: { active: true, hz: 82.4, note: 'E', octave: 2, cents: -2 } });
    expect(overlay.classList.contains('in-tune')).toBe(true);

    juce.fromBackend('amp', { v: 1, event: 'tuner', data: { active: false, hz: 0, note: '', octave: 0, cents: 0 } });
    expect(overlay.querySelector('.tuner-note')!.textContent).toBe('—');
    expect(overlay.querySelector('.tuner-octave')!.textContent).toBe('');
    expect(overlay.querySelector('.tuner-hz')!.textContent).toBe('');
    expect(overlay.classList.contains('in-tune')).toBe(false);

    key('Escape');
    const off = lastCommand();
    expect(off).toMatchObject({ cmd: 'setTuner', args: { on: false } });
    juce.reply(off.id, true, { on: false });
    await flush();
    expect(overlay.hidden).toBe(true);
  });

  it('leaving the stage switches the tuner off (once)', async () => {
    await started(makeRig(), { tunerOn: true });
    unmount = stage.renderStage(root);
    expect(root.querySelector<HTMLElement>('.tuner-overlay')!.hidden).toBe(false);
    state.setView('edit');
    juce.fromBackend('amp', { v: 1, event: 'rigDirty', data: { dirty: true } }); // more store updates meanwhile
    const offs = juce.commands().filter((c) => c.cmd === 'setTuner');
    expect(offs).toHaveLength(1);
    expect(offs[0].args).toEqual({ on: false });
    juce.reply(offs[0].id, true, { on: false });
    await flush();
    expect(state.store.get().tunerOn).toBe(false);
  });

  it('✎ Edit switches to the edit view', async () => {
    await started();
    unmount = stage.renderStage(root);
    root.querySelector<HTMLButtonElement>('.stage-edit')!.click();
    expect(state.store.get().view).toBe('edit');
  });

  it('shows a hint when no rig is loaded', async () => {
    await started(null);
    unmount = stage.renderStage(root);
    expect(root.textContent).toContain('No rig loaded');
  });
});

describe('PeakMeter', () => {
  it('maps -60..0 dBFS to 0..100 %, holds the peak ~1 s and flags clipping above -1 dBFS', () => {
    const m = new stage.PeakMeter();
    expect(m.update(-100, 0)).toMatchObject({ level: 0, peak: 0, clip: false });
    expect(m.update(-12, 100)).toMatchObject({ level: 80, peak: 80, clip: false });
    expect(m.update(-30, 600)).toMatchObject({ level: 50, peak: 80 }); // held
    expect(m.update(-30, 1200).peak).toBe(50); // released after ~1 s
    expect(m.update(-0.5, 1300).clip).toBe(true);
    expect(m.update(-40, 1500).clip).toBe(true); // the clip light holds too
    expect(m.update(-40, 2500).clip).toBe(false);
    expect(m.update(6, 2600).level).toBe(100);
  });
});
