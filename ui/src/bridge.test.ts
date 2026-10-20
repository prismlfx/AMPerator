import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { installFakeJuce, type FakeJuce } from './test-support';

let juce: FakeJuce;
let bridge: typeof import('./bridge');

beforeEach(async () => {
  vi.resetModules(); // fresh pending-call table and listener state per test
  juce = installFakeJuce();
  bridge = await import('./bridge');
});

afterEach(() => {
  vi.useRealTimers();
  delete (window as any).__JUCE__;
});

describe('call', () => {
  it('sends a v1 command envelope through the "amp" native function', () => {
    void bridge.call('loadRig', { id: 'abc' });
    expect(juce.commands()).toEqual([{ v: 1, cmd: 'loadRig', id: 1, args: { id: 'abc' } }]);
  });

  it('resolves on matching reply id', async () => {
    const first = bridge.call<{ n: number }>('getState');
    const second = bridge.call<{ n: number }>('listRigs');
    const [a, b] = juce.commands();
    juce.reply(b.id, true, { n: 2 }); // out of order
    juce.reply(999, true, { n: 999 }); // unknown id: ignored
    juce.reply(a.id, true, { n: 1 });
    await expect(first).resolves.toEqual({ n: 1 });
    await expect(second).resolves.toEqual({ n: 2 });
  });

  it('rejects on ok:false with error message', async () => {
    const p = bridge.call('loadRig', { id: 'nope' });
    juce.reply(juce.commands()[0].id, false, 'unknown rig: nope');
    await expect(p).rejects.toThrow('unknown rig: nope');
  });

  it('rejects after 10 s without a reply', async () => {
    vi.useFakeTimers();
    const p = bridge.call('saveRig');
    const settled = vi.fn();
    p.catch(settled);
    vi.advanceTimersByTime(9_999);
    await Promise.resolve();
    expect(settled).not.toHaveBeenCalled();
    vi.advanceTimersByTime(1);
    await expect(p).rejects.toThrow(/saveRig.*10 s/);
    // a late reply is ignored
    juce.reply(juce.commands()[0].id, true, {});
  });

  it('rejects when the page is not hosted by the app', async () => {
    delete (window as any).__JUCE__;
    await expect(bridge.call('getState')).rejects.toThrow(/not running inside AMPerator/);
  });
});

describe('on', () => {
  it('on() receives events and unsubscribe stops them', () => {
    const fn = vi.fn();
    const other = vi.fn();
    const off = bridge.on('rigDirty', fn);
    bridge.on('meter', other);

    juce.fromBackend('amp', { v: 1, event: 'rigDirty', data: { dirty: true } });
    expect(fn).toHaveBeenCalledWith({ dirty: true });
    expect(other).not.toHaveBeenCalled();

    off();
    juce.fromBackend('amp', { v: 1, event: 'rigDirty', data: { dirty: false } });
    expect(fn).toHaveBeenCalledTimes(1);
  });

  it('a throwing listener does not stop the others', () => {
    const spy = vi.spyOn(console, 'error').mockImplementation(() => {});
    const good = vi.fn();
    bridge.on('error', () => {
      throw new Error('boom');
    });
    bridge.on('error', good);
    juce.fromBackend('amp', { v: 1, event: 'error', data: { message: 'x', context: 'swapPlan' } });
    expect(good).toHaveBeenCalledOnce();
    expect(spy).toHaveBeenCalled();
  });

  it('subscribes to the backend only once', () => {
    bridge.on('a', () => {});
    bridge.on('b', () => {});
    void bridge.call('getState');
    expect(juce.listenerCount('amp')).toBe(1);
  });
});

describe('invokeNative', () => {
  it('resolves with the native function result', async () => {
    const p = bridge.invokeNative('chooseFolder');
    const inv = juce.sent.find((s) => s.eventId === '__juce__invoke' && s.payload.name === 'chooseFolder')!;
    expect(inv.payload.params).toEqual([]);
    juce.fromBackend('__juce__complete', { promiseId: inv.payload.resultId, result: '/Users/me/Tones' });
    await expect(p).resolves.toBe('/Users/me/Tones');
  });
});
