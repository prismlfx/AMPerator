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

describe('TONE3000 section', () => {
  const user = { username: 'ada', displayName: 'Ada', avatarUrl: '', url: 'https://www.tone3000.com/ada' };
  const status = (over: object = {}) => ({ configured: true, connected: true, busy: false, remembered: true, user, ...over });
  const section = () => [...document.querySelectorAll('.settings-section')].find((x) => x.querySelector('h3')?.textContent === 'TONE3000')!;
  const sectionButton = (text: string) => [...section().querySelectorAll('button')].find((b) => b.textContent === text)!;
  const send = (tone3000: object) => juce.fromBackend('amp', { v: 1, event: 'tone3000Status', data: tone3000 });

  async function openedWith(tone3000: object, settings: object = baseSettings) {
    const state = await import('../state');
    const p = state.initStore();
    juce.reply(juce.commands().find((c) => c.cmd === 'getState').id, true, {
      protocol: 1, rig: null, report: {}, dirty: false, saved: false, learning: false, startupErrors: [], settings: {},
      deviceStatus: { status: 'Running', latencyMs: 8, xruns: 0 }, controlStatus: { hid: 'NotFound', exclusive: false, midiDevices: [] },
      tunerOn: false, favorites: [], recents: [], bands: [], tone3000,
    });
    await p;
    await opened(settings);
  }

  it('shows the status, Disconnect and the auto-fetch checkbox, which sends setSettings{tone3000AutoFetch}', async () => {
    await openedWith(status());
    expect(section().textContent).toContain('Connected as ada');
    expect(section().textContent).not.toContain('will not be remembered');
    expect(sectionButton('Disconnect')).toBeDefined();
    expect(section().querySelector('input[type="text"]')).toBeNull(); // the build has a client id
    const box = section().querySelector<HTMLInputElement>('input[type="checkbox"]')!;
    expect(section().textContent).toContain('Download missing TONE3000 models automatically when opening a rig');
    expect(box.checked).toBe(false);
    box.checked = true;
    box.dispatchEvent(new Event('change', { bubbles: true }));
    expect(lastCommand()).toMatchObject({ cmd: 'setSettings', args: { tone3000AutoFetch: true } });
    juce.reply(lastCommand().id, true, { ...baseSettings, tone3000AutoFetch: true });
    await flush();
    expect(section().querySelector<HTMLInputElement>('input[type="checkbox"]')!.checked).toBe(true);
  });

  it('Disconnect sends tone3000Disconnect', async () => {
    await openedWith(status());
    sectionButton('Disconnect').click();
    expect(lastCommand()).toMatchObject({ cmd: 'tone3000Disconnect' });
  });

  it('when signed out: Not connected and Connect TONE3000, which starts the sign-in', async () => {
    await openedWith(status({ connected: false, user: null }));
    expect(section().textContent).toContain('Not connected');
    sectionButton('Connect TONE3000').click();
    expect(lastCommand()).toMatchObject({ cmd: 'tone3000Connect' });
  });

  it('says when the sign-in will not be remembered', async () => {
    await openedWith(status({ remembered: false }));
    expect(section().textContent).toContain('Sign-in will not be remembered on this Mac.');
    const line = [...section().querySelectorAll('p')].find((p) => p.textContent === 'Sign-in will not be remembered on this Mac.')!;
    expect(line.className).toBe('muted'); // a styled class (.hint is only styled inside .settings-status)
  });

  it('follows status changes while open and stops after closing', async () => {
    await openedWith(status({ connected: false, user: null }));
    send(status());
    expect(section().textContent).toContain('Connected as ada');
    expect(sectionButton('Disconnect')).toBeDefined();
    document.querySelector<HTMLButtonElement>('.modal-settings [aria-label="Close"]')!.click(); // (an earlier test's modal may swallow Esc)
    expect(document.querySelector('.settings-section')).toBeNull();
    send(status({ connected: false, user: null })); // nothing left to update, and nothing throws
  });

  it('without a client id in the build: says so and offers the client-id field', async () => {
    await openedWith({ configured: false, connected: false, busy: false, remembered: true, user: null });
    expect(section().textContent).toContain('This build has no TONE3000 client id.');
    expect(section().textContent).toContain('Client id (takes effect after restarting AMPerator)');
    expect(sectionButton('Connect TONE3000')).toBeUndefined();
    const input = section().querySelector<HTMLInputElement>('input[type="text"]')!;
    input.value = ' pk_test_1 ';
    input.dispatchEvent(new Event('change', { bubbles: true }));
    expect(lastCommand()).toMatchObject({ cmd: 'setSettings', args: { tone3000ClientId: 'pk_test_1' } });
  });

  it('shows the client-id field when one is set, even in a configured build', async () => {
    await openedWith(status(), { ...baseSettings, tone3000ClientId: 'pk_mine' });
    expect(section().querySelector<HTMLInputElement>('input[type="text"]')!.value).toBe('pk_mine');
  });
});
