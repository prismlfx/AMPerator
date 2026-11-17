import { afterEach, describe, expect, it, vi } from 'vitest';
import { installFakeJuce, type FakeJuce } from './test-support';

let juce: FakeJuce;

const user = { username: 'ada', displayName: 'Ada', avatarUrl: '', url: 'https://www.tone3000.com/ada' };
const connected = { configured: true, connected: true, busy: false, remembered: true, user };
const signedOut = { configured: true, connected: false, busy: false, remembered: true, user: null };
const report = (fetchable: string[]) => ({ missingAssets: ['/m/a.nam'], missingPlugins: [], placeholderNodeIds: fetchable, resolvedByFilename: [], fetchable });
const rig = {
  id: 'r1', name: 'Rig', band: 'B', tags: [], connections: [], mappings: [],
  nodes: [
    { id: 'in', type: 'Input', bypass: false, params: {} },
    ...['n3', 'n4'].map((id) => ({ id, type: 'NamAmp', bypass: true, params: {}, asset: { kind: 'nam', file: `/m/${id}.nam`, sha256: id, source: { provider: 'tone3000', id: `m-${id}` } } })),
    { id: 'out', type: 'Output', bypass: false, params: {} },
  ],
};

const flush = () => new Promise((r) => setTimeout(r, 0));
const lastCommand = () => juce.commands().at(-1);
const banners = () => [...document.querySelectorAll('.banner')];
const t3kBanner = () => banners().find((b) => b.textContent!.includes('from TONE3000'));
const bannerButton = (text: string) => [...t3kBanner()!.querySelectorAll<HTMLButtonElement>('button')].find((b) => b.textContent === text)!;
const event = (name: string, data: unknown) => juce.fromBackend('amp', { v: 1, event: name, data });

async function boot(tone3000: object, fetchable: string[]) {
  vi.resetModules();
  document.body.innerHTML = '<div id="app"></div>';
  juce = installFakeJuce();
  await import('./main');
  juce.reply(juce.commands().find((c) => c.cmd === 'getState').id, true, {
    protocol: 1, rig, report: report(fetchable), dirty: false, saved: true, learning: false, startupErrors: [], settings: {},
    deviceStatus: { status: 'Running', latencyMs: 8, xruns: 0 }, controlStatus: { hid: 'NotFound', exclusive: false, midiDevices: [] },
    tunerOn: false, favorites: [], recents: [], bands: [], tone3000,
  });
  await flush();
}

afterEach(() => {
  delete (window as any).__JUCE__;
});

describe('missing TONE3000 models banner', () => {
  it('says how many models are missing and sends tone3000Fetch{} from Download when connected', async () => {
    await boot(connected, ['n3']);
    expect(t3kBanner()!.querySelector('.banner-text')!.textContent).toBe('1 model from TONE3000 is missing.');
    bannerButton('Download').click();
    await flush();
    expect(lastCommand()).toMatchObject({ cmd: 'tone3000Fetch', args: {} });
    expect(Object.keys(lastCommand().args)).toEqual([]);
  });

  it('uses the plural form', async () => {
    await boot(connected, ['n3', 'n4']);
    expect(t3kBanner()!.querySelector('.banner-text')!.textContent).toBe('2 models from TONE3000 are missing.');
  });

  it('signs in first when not connected, and fetches only once connected', async () => {
    await boot(signedOut, ['n3']);
    bannerButton('Download').click();
    await flush();
    expect(lastCommand()).toMatchObject({ cmd: 'tone3000Connect' });
    juce.reply(lastCommand().id, true, { connected: true });
    await flush();
    expect(lastCommand()).toMatchObject({ cmd: 'tone3000Fetch', args: {} });
  });

  it('does not fetch when the sign-in is canceled', async () => {
    await boot(signedOut, ['n3']);
    bannerButton('Download').click();
    await flush();
    juce.reply(lastCommand().id, true, { canceled: true });
    await flush();
    expect(juce.commands().some((c) => c.cmd === 'tone3000Fetch')).toBe(false);
    expect(bannerButton('Download').disabled).toBe(false);
  });

  it('is disabled and reads Downloading… while one of its models downloads, and enabled again after', async () => {
    await boot(connected, ['n3', 'n4']);
    event('tone3000Download', { jobId: 'j1', modelId: 'm-n3', toneId: 't', nodeId: 'n3', state: 'downloading', progress: 0.2 });
    expect(bannerButton('Downloading…').disabled).toBe(true);
    event('tone3000Download', { jobId: 'j1', modelId: 'm-n3', toneId: 't', nodeId: 'n3', state: 'done', progress: 1 });
    expect(bannerButton('Download').disabled).toBe(false);
  });

  it('progress ticks do not redraw the banners, a state change does', async () => {
    await boot(connected, ['n3', 'n4']);
    event('tone3000Download', { jobId: 'j1', modelId: 'm-n3', toneId: 't', nodeId: 'n3', state: 'downloading', progress: 0.2 });
    const drawn = t3kBanner();
    event('tone3000Download', { jobId: 'j1', modelId: 'm-n3', toneId: 't', nodeId: 'n3', state: 'downloading', progress: 0.6 });
    expect(t3kBanner()).toBe(drawn); // the same element: a Dismiss click on it is not lost
    event('tone3000Download', { jobId: 'j1', modelId: 'm-n3', toneId: 't', nodeId: 'n3', state: 'done', progress: 1 });
    expect(bannerButton('Download').disabled).toBe(false);
  });

  it('shows the error when the fetch fails', async () => {
    await boot(connected, ['n3']);
    bannerButton('Download').click();
    await flush();
    juce.reply(lastCommand().id, false, 'rate limited');
    await flush();
    expect(document.getElementById('toasts')!.textContent).toContain('rate limited');
    expect(bannerButton('Download').disabled).toBe(false);
  });

  it('can be dismissed, and the generic missing-assets banner stays separate', async () => {
    await boot(connected, ['n3']);
    expect(banners().length).toBe(2);
    t3kBanner()!.querySelector<HTMLButtonElement>('[aria-label="Dismiss"]')!.click();
    expect(t3kBanner()).toBeUndefined();
    expect(banners().length).toBe(1);
  });

  it('is absent when nothing is fetchable or the integration is not configured', async () => {
    await boot(connected, []);
    expect(t3kBanner()).toBeUndefined();
    await boot({ configured: false, connected: false, busy: false, remembered: true, user: null }, ['n3']);
    expect(t3kBanner()).toBeUndefined();
  });
});
