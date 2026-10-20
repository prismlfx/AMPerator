import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { installFakeJuce, type FakeJuce } from './test-support';

let juce: FakeJuce;
let state: typeof import('./state');

const rig = {
  id: 'r1',
  name: 'Clean',
  band: 'My Tones',
  nodes: [
    { id: 'in', type: 'Input', bypass: false, params: {} },
    { id: 'g', type: 'Gate', bypass: false, params: { threshold: -60 } },
    { id: 'out', type: 'Output', bypass: false, params: {} },
  ],
};

function getStateResult(overrides: object = {}) {
  return {
    protocol: 1,
    rig,
    report: { missingAssets: [], missingPlugins: [], placeholderNodeIds: [], resolvedByFilename: [] },
    dirty: false,
    saved: true,
    learning: false,
    startupErrors: [],
    settings: { bufferSize: 128 },
    deviceStatus: { status: 'Running', latencyMs: 8.1, xruns: 0 },
    controlStatus: { hid: 'NotFound', exclusive: false, midiDevices: [] },
    tunerOn: false,
    favorites: [],
    recents: [],
    bands: ['My Tones'],
    ...overrides,
  };
}

async function started(overrides: object = {}) {
  const p = state.initStore();
  juce.reply(juce.commands()[0].id, true, getStateResult(overrides));
  await p;
}

beforeEach(async () => {
  vi.resetModules();
  juce = installFakeJuce();
  state = await import('./state');
});

afterEach(() => {
  delete (window as any).__JUCE__;
});

describe('store', () => {
  it('initStore loads getState into the store', async () => {
    expect(state.store.get().connected).toBe(false);
    await started();
    expect(juce.commands()[0].cmd).toBe('getState');
    const s = state.store.get();
    expect(s.connected).toBe(true);
    expect(s.rig?.name).toBe('Clean');
    expect(s.deviceStatus.latencyMs).toBe(8.1);
    expect(s.view).toBe('edit');
  });

  it('updates from events and notifies subscribers; unsubscribe stops them', async () => {
    await started();
    const fn = vi.fn();
    const off = state.store.subscribe(fn);

    juce.fromBackend('amp', { v: 1, event: 'rigDirty', data: { dirty: true } });
    expect(state.store.get().dirty).toBe(true);
    expect(fn).toHaveBeenCalledTimes(1);

    juce.fromBackend('amp', { v: 1, event: 'deviceStatus', data: { status: 'Running', latencyMs: 4.2, xruns: 3 } });
    expect(state.store.get().deviceStatus).toEqual({ status: 'Running', latencyMs: 4.2, xruns: 3 });

    juce.fromBackend('amp', { v: 1, event: 'bypassChanged', data: { id: 'g', bypass: true } });
    expect(state.store.get().rig!.nodes[1].bypass).toBe(true);

    juce.fromBackend('amp', { v: 1, event: 'paramChanged', data: { id: 'g', name: 'threshold', value: -40 } });
    expect(state.store.get().rig!.nodes[1].params.threshold).toBe(-40);

    juce.fromBackend('amp', {
      v: 1,
      event: 'controlStatus',
      data: { hid: 'Connected', exclusive: true, midiDevices: ['Pedal'] },
    });
    expect(state.store.get().controlStatus.hid).toBe('Connected');

    off();
    juce.fromBackend('amp', { v: 1, event: 'rigDirty', data: { dirty: false } });
    expect(fn).toHaveBeenCalledTimes(5);
    expect(state.store.get().dirty).toBe(false);
  });

  it('rigLoaded replaces rig, report and saved', async () => {
    await started();
    const other = { ...rig, id: 'r2', name: 'Lead' };
    juce.fromBackend('amp', {
      v: 1,
      event: 'rigLoaded',
      data: { rig: other, report: { missingAssets: ['a.nam'], missingPlugins: [], placeholderNodeIds: [], resolvedByFilename: [] }, saved: false },
    });
    const s = state.store.get();
    expect(s.rig!.name).toBe('Lead');
    expect(s.report.missingAssets).toEqual(['a.nam']);
    expect(s.saved).toBe(false);
  });

  it('dismissStartupError removes one banner', async () => {
    await started({
      startupErrors: [
        { message: 'bad json', context: 'loadRig', rigId: 'r9' },
        { message: 'no device', context: 'audioDevice' },
      ],
    });
    state.dismissStartupError(0);
    expect(state.store.get().startupErrors).toEqual([{ message: 'no device', context: 'audioDevice' }]);
  });
});

describe('confirmDiscardIfDirty', () => {
  it('resolves true without asking when the rig is clean', async () => {
    await started();
    const ask = vi.fn();
    await expect(state.confirmDiscardIfDirty(ask)).resolves.toBe(true);
    expect(ask).not.toHaveBeenCalled();
  });

  it('asks when dirty and returns the answer', async () => {
    await started({ dirty: true });
    const no = vi.fn().mockResolvedValue(false);
    await expect(state.confirmDiscardIfDirty(no)).resolves.toBe(false);
    expect(no.mock.calls[0][0]).toContain('Clean');
    await expect(state.confirmDiscardIfDirty(async () => true)).resolves.toBe(true);
  });
});

describe('onToastableError', () => {
  const err = (data: object) => juce.fromBackend('amp', { v: 1, event: 'error', data });

  it('skips errors before getState answered and errors already shown as startup banners', async () => {
    const toast = vi.fn();
    state.onToastableError(toast);
    const p = state.initStore();
    err({ message: 'no device', context: 'audioDevice' }); // queued startup error, delivered before the reply
    juce.reply(juce.commands()[0].id, true, getStateResult({ startupErrors: [{ message: 'no device', context: 'audioDevice' }] }));
    await p;
    err({ message: 'no device', context: 'audioDevice' }); // duplicate of a banner
    expect(toast).not.toHaveBeenCalled();

    err({ message: 'swap failed', context: 'swapPlan' });
    expect(toast).toHaveBeenCalledWith({ message: 'swap failed', context: 'swapPlan' });
  });

  it('turns a late audioPermission error into a startup banner instead of a toast', async () => {
    const toast = vi.fn();
    state.onToastableError(toast);
    await started();
    err({ message: 'Microphone access denied', context: 'audioPermission' });
    err({ message: 'Microphone access denied', context: 'audioPermission' });
    expect(toast).not.toHaveBeenCalled();
    expect(state.store.get().startupErrors).toEqual([{ message: 'Microphone access denied', context: 'audioPermission' }]);
  });
});

describe('rigSaved', () => {
  it('applies the saved name/band and marks an unsaved rig as saved', async () => {
    await started({ saved: false });
    state.rigSaved('Black Dog', 'Zeppelin');
    const s = state.store.get();
    expect([s.rig?.name, s.rig?.band, s.saved]).toEqual(['Black Dog', 'Zeppelin', true]);
  });
});
