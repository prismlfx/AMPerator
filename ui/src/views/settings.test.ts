import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { installFakeJuce, type FakeJuce } from '../test-support';

let juce: FakeJuce;
let settingsView: typeof import('./settings');

const baseSettings = {
  audioDeviceType: 'CoreAudio',
  audioDeviceName: 'Scarlett Solo USB',
  inputChannel: 0,
  outputFirstChannel: 0,
  sampleRate: 48000,
  bufferSize: 128,
  watchedFolders: ['/a', '/b'],
  globalMappings: [],
  footswitchKeys: {},
  availableDevices: ['Scarlett Solo USB'],
  availableSampleRates: [48000],
  availableBufferSizes: [64, 128],
  availableInputChannels: ['In 1'],
  availableOutputChannels: ['Out 1', 'Out 2'],
};

const flush = () => new Promise((r) => setTimeout(r, 0));
const lastCommand = () => juce.commands().at(-1);
const folderButtons = () => [...document.querySelectorAll<HTMLButtonElement>('.settings-folders button')];
const removeButton = (path: string) =>
  [...document.querySelectorAll('.folder-list li')].find((li) => li.textContent!.startsWith(path))!.querySelector('button')!;

beforeEach(async () => {
  vi.resetModules();
  document.body.innerHTML = '';
  juce = installFakeJuce();
  settingsView = await import('./settings');
});

afterEach(() => {
  document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape' })); // close the modal
  delete (window as any).__JUCE__;
});

const command = (name: string) => juce.commands().filter((c) => c.cmd === name).at(-1);

async function opened(settings: object = baseSettings) {
  settingsView.openSettings();
  const get = command('getSettings');
  expect(get).toBeDefined();
  juce.reply(get.id, true, settings);
  await flush();
}

describe('watched folders', () => {
  it('disables folder buttons while a settings change is in flight', async () => {
    await opened();
    removeButton('/a').click();
    expect(lastCommand()).toMatchObject({ cmd: 'setSettings', args: { watchedFolders: ['/b'] } });
    expect(folderButtons().length).toBeGreaterThan(0);
    expect(folderButtons().every((b) => b.disabled)).toBe(true);

    juce.reply(lastCommand().id, true, { ...baseSettings, watchedFolders: ['/b'] });
    await flush();
    expect(folderButtons().every((b) => !b.disabled)).toBe(true);
  });

  it('builds the patch from the latest settings, not the list the buttons were rendered with', async () => {
    await opened();
    folderButtons().find((b) => b.textContent === 'Add folder…')!.click(); // chooser stays open...
    await flush();
    removeButton('/a').click(); // ...while another change goes through
    juce.reply(lastCommand().id, true, { ...baseSettings, watchedFolders: ['/b'] });
    await flush();
    const invoke = juce.sent.filter((s) => s.eventId === '__juce__invoke' && s.payload.name === 'chooseFolder').at(-1)!;
    juce.fromBackend('__juce__complete', { promiseId: invoke.payload.resultId, result: '/d' });
    await flush();
    expect(lastCommand()).toMatchObject({ cmd: 'setSettings', args: { watchedFolders: ['/b', '/d'] } });
  });
});

describe('status texts', () => {
  const text = (nodes: Node[]) => nodes.map((n) => n.textContent).join(' ');

  it('explains how to grant Input Monitoring when permission is denied', () => {
    const t = text(settingsView.controlStatusView({ hid: 'PermissionDenied', exclusive: false, midiDevices: [] }));
    expect(t).toContain('Open System Settings → Privacy & Security → Input Monitoring, enable AMPerator, then relaunch');
  });

  it('gives the F13–F15 tip for a shared footswitch, not for an exclusive one', () => {
    expect(text(settingsView.controlStatusView({ hid: 'Connected', exclusive: false, midiDevices: [] }))).toContain(
      'shared: presses also type into other apps — tip: set your pedal to send F13/F14/F15',
    );
    expect(text(settingsView.controlStatusView({ hid: 'Connected', exclusive: true, midiDevices: [] }))).toContain('exclusive');
  });

  it('reports a busy footswitch and lists MIDI devices', () => {
    const t = text(settingsView.controlStatusView({ hid: 'Busy', exclusive: false, midiDevices: ['Pedal X'] }));
    expect(t).toContain('In use by another app');
    expect(t).toContain('Pedal X');
  });

  it('shows latency and xruns, with a hint when xruns occurred', () => {
    const t = text(settingsView.deviceStatusView({ status: 'Running', latencyMs: 4.06, xruns: 3 }));
    expect(t).toContain('4.1 ms');
    expect(t).toContain('larger buffer size');
    expect(text(settingsView.deviceStatusView({ status: 'NoDevice', latencyMs: 0, xruns: -1 }))).toContain('No audio device open');
  });
});

describe('footswitch key assignment', () => {
  const connected = { hid: 'Connected' as const, exclusive: true, midiDevices: [] };
  const buttons = (nodes: Node[]) => nodes.flatMap((n) => [...(n as Element).querySelectorAll?.('button') ?? []]);

  it('shows the last key and an Assign button per pedal that calls the bridge', () => {
    const assign = vi.fn();
    const nodes = settingsView.controlStatusView({ ...connected, lastKey: 5 }, { assign, cancel: vi.fn() });
    expect(nodes.map((n) => n.textContent).join(' ')).toContain('Last key: 0x05');
    const bs = buttons(nodes);
    expect(bs.map((b) => b.textContent)).toEqual(['Assign L', 'Assign M', 'Assign R']);
    bs[2].click();
    expect(assign).toHaveBeenCalledWith('footswitch:R');
  });

  it('prompts while waiting and a second click cancels', () => {
    const cancel = vi.fn();
    const nodes = settingsView.controlStatusView({ ...connected, lastKey: null, assigning: 'footswitch:M' }, { assign: vi.fn(), cancel });
    const text = nodes.map((n) => n.textContent).join(' ');
    expect(text).toContain('Last key: none yet');
    const bs = buttons(nodes);
    expect(bs[1].textContent).toBe('Press the pedal now…');
    bs[1].click();
    expect(cancel).toHaveBeenCalled();
  });

  it('Esc in the open dialog cancels a pending assign instead of closing it', async () => {
    await opened();
    const state = await import('../state');
    const started = state.initStore();
    juce.reply(lastCommand().id, true, { controlStatus: { ...connected, lastKey: null, assigning: 'footswitch:L' } });
    await started;
    await flush();
    document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true }));
    await flush();
    expect(lastCommand()).toMatchObject({ cmd: 'cancelAssignFootswitchKey' });
    expect(document.querySelector('.modal-settings')).not.toBeNull();
  });

  it('closing the dialog mid-assign cancels the assign', async () => {
    await opened();
    const state = await import('../state');
    const started = state.initStore();
    juce.reply(lastCommand().id, true, { controlStatus: { ...connected, lastKey: null, assigning: 'footswitch:L' } });
    await started;
    await flush();
    document.querySelector<HTMLElement>('.modal-settings button[aria-label="Close"], .modal-settings .modal-close')?.click();
    await flush();
    expect(juce.commands().some((c) => c.cmd === 'cancelAssignFootswitchKey')).toBe(true);
  });
});

describe('disabled plugins', () => {
  it('lists blacklisted plugins and Retry re-enables one', async () => {
    await opened();
    juce.reply(command('listBlacklisted').id, true, { plugins: [{ id: 'VST3-Bad-1', name: 'Bad Synth' }] });
    await flush();
    const section = document.querySelector('.settings-disabled')!;
    expect(section.textContent).toContain('Bad Synth');
    const retry = [...section.querySelectorAll('button')].find((b) => b.textContent === 'Retry')!;
    retry.click();
    expect(lastCommand()).toMatchObject({ cmd: 'unblacklistPlugin', args: { id: 'VST3-Bad-1' } });
    juce.reply(lastCommand().id, true, { plugins: [] });
    await flush();
    expect(section.textContent).toContain('No disabled plugins');
  });
});

describe('footswitch device', () => {
  const keyboards = [
    { vid: 0x1234, pid: 0x5678, id: '1234:5678', manufacturer: 'Acme', product: 'Pedal' },
    { vid: 0x05ac, pid: 0x0342, id: '05ac:0342', manufacturer: 'Apple', product: 'Internal Keyboard' },
  ];
  const deviceButtons = () => [...document.querySelectorAll<HTMLButtonElement>('.footswitch-device button')];

  it('lists connected keyboards on request and uses the chosen one as the footswitch', async () => {
    await opened();
    deviceButtons().find((b) => b.textContent === 'Choose device…')!.click();
    expect(lastCommand().cmd).toBe('listHidKeyboards');
    juce.reply(lastCommand().id, true, { keyboards });
    await flush();
    expect(document.querySelector('.footswitch-device')!.textContent).toContain('Acme Pedal');
    deviceButtons().filter((b) => b.textContent === 'Use as footswitch')[0].click();
    expect(lastCommand()).toMatchObject({ cmd: 'setSettings', args: { footswitchDevice: { vid: 0x1234, pid: 0x5678 } } });
  });

  it('shows the chosen device and can clear it', async () => {
    await opened({ ...baseSettings, footswitchDevice: { vid: 0x1234, pid: 0x5678 } });
    expect(document.querySelector('.footswitch-device')!.textContent).toContain('1234:5678');
    deviceButtons().find((b) => b.textContent === 'Clear')!.click();
    expect(lastCommand()).toMatchObject({ cmd: 'setSettings', args: { footswitchDevice: null } });
  });
});
