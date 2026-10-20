import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { installFakeJuce, type FakeJuce } from './test-support';

let juce: FakeJuce;
let actions: typeof import('./rigActions');
let state: typeof import('./state');

const flush = () => new Promise((r) => setTimeout(r, 0));
const lastCommand = () => juce.commands().at(-1);

const rig = { id: 'r1', name: 'Lead', band: 'Band', nodes: [] };

beforeEach(async () => {
  vi.resetModules();
  document.body.innerHTML = '';
  juce = installFakeJuce();
  state = await import('./state');
  actions = await import('./rigActions');
  const p = state.initStore();
  juce.reply(juce.commands()[0].id, true, {
    rig,
    report: { missingAssets: [], missingPlugins: [], placeholderNodeIds: [], resolvedByFilename: [] },
    dirty: true,
    saved: true,
  });
  await p;
});

afterEach(() => {
  delete (window as any).__JUCE__;
});

describe('quit prompt', () => {
  it.each(['save', 'discard', 'cancel'] as const)('answers quitRequested with confirmQuit{%s}', async (choice) => {
    const ask = vi.fn(async () => choice);
    actions.watchQuitRequests(ask);
    juce.fromBackend('amp', { v: 1, event: 'quitRequested', data: { rigName: 'Lead' } });
    await flush();
    expect(ask).toHaveBeenCalledWith('Lead');
    expect(lastCommand()).toMatchObject({ cmd: 'confirmQuit', args: { action: choice } });
  });

  it('the default prompt offers Save, Discard and Cancel', async () => {
    actions.watchQuitRequests();
    juce.fromBackend('amp', { v: 1, event: 'quitRequested', data: { rigName: 'Lead' } });
    const labels = [...document.querySelectorAll('.modal button')].map((b) => b.textContent);
    expect(labels).toEqual(expect.arrayContaining(['Save', 'Discard', 'Cancel']));
    [...document.querySelectorAll<HTMLButtonElement>('.modal button')].find((b) => b.textContent === 'Discard')!.click();
    await flush();
    expect(lastCommand()).toMatchObject({ cmd: 'confirmQuit', args: { action: 'discard' } });
  });
});

describe('save as', () => {
  it('prefills "<name> copy" and the band, then saves a new rig', async () => {
    const ask = vi.fn(async (d: { name: string; band: string }) => ({ name: d.name, band: 'Other' }));
    const done = actions.saveRigAs(ask);
    await flush();
    expect(ask).toHaveBeenCalledWith({ name: 'Lead copy', band: 'Band' });
    expect(lastCommand()).toMatchObject({ cmd: 'saveRig', args: { asNew: true, name: 'Lead copy', band: 'Other' } });
    juce.reply(lastCommand().id, true, { id: 'r2', name: 'Lead copy', band: 'Other' });
    await done;
  });

  it('cancelling the prompt saves nothing', async () => {
    await actions.saveRigAs(async () => null);
    expect(juce.commands().map((c) => c.cmd)).toEqual(['getState']);
  });
});

describe('disabled plugin retry', () => {
  it('unblacklists the plugin and resolves true', async () => {
    const done = actions.retryBlacklistedPlugin('VST3-Bad-1');
    expect(lastCommand()).toMatchObject({ cmd: 'unblacklistPlugin', args: { id: 'VST3-Bad-1' } });
    juce.reply(lastCommand().id, true, { plugins: [] });
    expect(await done).toBe(true);
    expect(document.querySelector('.toast')!.textContent).toContain('Rescan');
  });
});
