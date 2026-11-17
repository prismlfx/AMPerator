import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { installFakeJuce, type FakeJuce } from '../test-support';

let juce: FakeJuce;
let state: typeof import('../state');
let view: typeof import('./tone3000');
let online: typeof import('../online/tone3000');
let content: HTMLElement;
let dispose: () => void;

const emptyReport = { missingAssets: [], missingPlugins: [], placeholderNodeIds: [], resolvedByFilename: [], fetchable: [] };
const user = { username: 'ada', displayName: 'Ada L', avatarUrl: 'https://cdn.example/ada.png', url: 'https://www.tone3000.com/ada' };
const connected = { configured: true, connected: true, busy: false, remembered: true, user };
const signedOut = { configured: true, connected: false, busy: false, remembered: false, user: null };

const tone = (id: string, over: object = {}) => ({
  id,
  title: `Tone ${id}`,
  gear: 'amp',
  format: 'nam',
  license: 'cc-by',
  creator: user,
  image: '',
  url: `https://www.tone3000.com/tones/${id}`,
  modelsCount: 2,
  downloadsCount: 5,
  favoritesCount: 1,
  ...over,
});

const flush = () => new Promise((r) => setTimeout(r, 0));
const commands = (name: string) => juce.commands().filter((c) => c.cmd === name);
const last = (name: string) => commands(name).at(-1);
const event = (name: string, data: unknown) => juce.fromBackend('amp', { v: 1, event: name, data });
const button = (text: string) => [...content.querySelectorAll<HTMLButtonElement>('button')].find((b) => b.textContent === text)!;
const natives = (name: string) => juce.sent.filter((s) => s.eventId === '__juce__invoke' && s.payload.name === name);

async function started(tone3000: object, rig: object | null = null) {
  const p = state.initStore();
  juce.reply(juce.commands().find((c) => c.cmd === 'getState').id, true, {
    protocol: 1, rig, report: emptyReport, dirty: false, saved: false, learning: false, startupErrors: [], settings: {},
    deviceStatus: { status: 'Running', latencyMs: 8, xruns: 0 }, controlStatus: { hid: 'NotFound', exclusive: false, midiDevices: [] },
    tunerOn: false, favorites: [], recents: [], bands: [], tone3000,
  });
  await p;
}

/** Renders the tab and answers its listAssets (the library index) and tone3000List requests. */
async function opened(status: object, tones = [tone('1'), tone('2', { format: 'ir', title: 'Fillmore 4x12' })], assets: object[] = []) {
  await started(status);
  dispose = view.renderTone3000Tab(content);
  if (!(status as any).connected) return;
  await answerLists(tones, assets);
}

async function answerLists(tones: object[], assets: object[] = [], page = 1, totalPages = 1) {
  await flush();
  juce.reply(last('tone3000List').id, true, { tones, page, totalPages });
  for (const c of commands('listAssets').slice(-2)) juce.reply(c.id, true, { assets: c.args.kind === 'nam' ? assets : [] });
  await flush();
}

beforeEach(async () => {
  vi.resetModules();
  document.body.innerHTML = '';
  juce = installFakeJuce();
  state = await import('../state');
  view = await import('./tone3000');
  online = await import('../online/tone3000');
  content = document.createElement('div');
  document.body.append(content);
});

afterEach(() => {
  dispose?.();
  delete (window as any).__JUCE__;
});

describe('signed out', () => {
  it('says so when the build has no client id', async () => {
    await opened({ configured: false, connected: false, busy: false, remembered: false, user: null });
    expect(content.querySelector('p.muted')!.textContent).toBe('This build has no TONE3000 client id.');
  });

  it('shows the splash; Connect TONE3000 sends tone3000Connect with a long timeout', async () => {
    await opened(signedOut);
    expect(content.textContent).toContain('AMPerator has partnered with TONE3000 to give you access to thousands of NAM captures and IRs.');
    expect(content.querySelector('img')!.alt).toBe('TONE3000');
    button('Connect TONE3000').click();
    expect(last('tone3000Connect')).toBeDefined();
    juce.reply(last('tone3000Connect').id, true, { canceled: true });
    await flush();
    expect(document.getElementById('toasts')).toBeNull(); // a canceled sign-in is not an error
  });

  it('"Powered by TONE3000" opens the site through openExternal', async () => {
    await opened(signedOut);
    button('Powered by TONE3000').click();
    expect(natives('openExternal').at(-1)!.payload.params).toEqual(['https://www.tone3000.com']);
    expect(content.querySelector('a')).toBeNull();
  });

  it('a connect error is toasted', async () => {
    await opened(signedOut);
    button('Connect TONE3000').click();
    juce.reply(last('tone3000Connect').id, false, "Can't reach TONE3000: offline");
    await flush();
    expect(document.getElementById('toasts')!.textContent).toContain("Can't reach TONE3000: offline");
  });
});

describe('signed in', () => {
  it('requests the favorites first and renders the rows', async () => {
    await opened(connected, [tone('1', { image: 'https://cdn.example/1.jpg' }), tone('2', { format: 'ir', title: 'Fillmore 4x12' })]);
    expect(last('tone3000List')).toMatchObject({ args: { list: 'favorited', page: 1 } });
    const rows = [...content.querySelectorAll('li.tone-row')];
    expect(rows).toHaveLength(2);
    expect(rows[0].querySelector('.tone-title')!.textContent).toBe('Tone 1');
    expect(rows[0].querySelector('.tone-creator')!.textContent).toBe('ada');
    expect(rows[0].querySelector('.format-badge')!.textContent).toBe('NAM');
    const art = rows[0].querySelector<HTMLImageElement>('img.tone-image')!;
    expect(art.getAttribute('src')).toBe('https://cdn.example/1.jpg');
    expect(art.getAttribute('referrerpolicy')).toBe('no-referrer');
    expect(art.getAttribute('loading')).toBe('lazy');
    expect(art.alt).toBe('');
    expect(rows[1].querySelector('.format-badge')!.textContent).toBe('IR');
    expect(rows[1].querySelector<HTMLImageElement>('img.tone-image')!.src).toMatch(/^data:image\/svg\+xml/); // archetype
    expect(content.querySelector('.tone-account')!.textContent).toContain('ada');
  });

  it('only https images are shown', async () => {
    await opened(connected, [tone('1', { image: 'http://x/1.jpg', creator: { ...user, avatarUrl: 'javascript:alert(1)' } })]);
    const row = content.querySelector('li.tone-row')!;
    expect(row.querySelector<HTMLImageElement>('img.tone-image')!.src).toMatch(/^data:/);
    expect(row.querySelector('img.avatar')).toBeNull();
  });

  it('marks the tones whose models are in the library with T3K', async () => {
    await opened(connected, [tone('1'), tone('2')], [{ name: 'm', file: '/x', source: { provider: 'tone3000', id: '77', toneId: '2' } }]);
    const rows = [...content.querySelectorAll('li.tone-row')];
    expect(rows[0].querySelector('.tone-mark')).toBeNull();
    expect(rows[1].querySelector<HTMLImageElement>('.tone-mark')!.alt).toBe('T3K');
  });

  it('Browse TONE3000 sends no format by default and the active chip otherwise', async () => {
    await opened(connected);
    button('Browse TONE3000').click();
    expect(last('tone3000Browse').args).toEqual({});
    juce.reply(last('tone3000Browse').id, true, { canceled: true });
    await flush();

    const ir = button('IR');
    ir.click();
    expect(ir.getAttribute('aria-pressed')).toBe('true');
    button('NAM').click();
    expect(ir.getAttribute('aria-pressed')).toBe('false');
    button('NAM').click(); // off again
    ir.click();
    button('Browse TONE3000').click();
    expect(last('tone3000Browse').args).toEqual({ format: 'ir' });
  });

  it('a Browse result opens the tone detail', async () => {
    await opened(connected);
    button('Discover more on TONE3000').click();
    juce.reply(last('tone3000Browse').id, true, { toneId: '9' });
    await flush();
    expect(last('tone3000Tone')).toMatchObject({ args: { id: '9' } });
    juce.reply(last('tone3000Tone').id, true, { ...tone('9', { title: 'Picked tone' }), models: [] });
    for (const c of commands('listAssets').slice(-2)) juce.reply(c.id, true, { assets: [] });
    await flush();
    expect(document.querySelector('.modal')!.getAttribute('aria-label')).toBe('Picked tone');
    document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape' }));
  });

  it('switches lists with the sub-tabs and pages with Previous / Next', async () => {
    await opened(connected);
    button('Trending').click();
    await answerLists([tone('3')], [], 1, 3);
    expect(last('tone3000List').args).toEqual({ list: 'trending', page: 1 });
    expect(button('Previous').disabled).toBe(true);
    button('Next').click();
    expect(last('tone3000List').args).toEqual({ list: 'trending', page: 2 });
    await answerLists([tone('4')], [], 2, 3);
    expect(content.querySelector('.tone-title')!.textContent).toBe('Tone 4');
    expect(button('Previous').disabled).toBe(false);
  });

  it('shows an error in place and Retry asks again', async () => {
    await opened(connected);
    button('Latest').click();
    await flush();
    juce.reply(last('tone3000List').id, false, 'TONE3000 rate limit; try again in 12 s');
    await flush();
    expect(content.querySelector('.tone-error')!.textContent).toContain('TONE3000 rate limit; try again in 12 s');
    const before = commands('tone3000List').length;
    button('Retry').click();
    expect(commands('tone3000List')).toHaveLength(before + 1);
    await answerLists([tone('5')]);
    expect(content.querySelector('.tone-error')).toBeNull();
  });

  it('Disconnect sends tone3000Disconnect', async () => {
    await opened(connected);
    button('Disconnect').click();
    expect(last('tone3000Disconnect')).toBeDefined();
  });

  it('a tone3000Status event signing out returns to the splash', async () => {
    await opened(connected);
    event('tone3000Status', signedOut);
    expect(button('Connect TONE3000')).toBeDefined();
    expect(content.querySelector('.tone-row')).toBeNull();
  });

  it('a busy-only status change keeps the lists and disables Browse', async () => {
    await opened(connected);
    event('tone3000Status', { ...connected, busy: true });
    expect(content.querySelectorAll('li.tone-row')).toHaveLength(2);
    expect(button('Browse TONE3000').disabled).toBe(true);
  });

  it('dispose drops the subscriptions', async () => {
    await opened(signedOut);
    dispose();
    event('tone3000Status', connected);
    expect(content.querySelector('.tone-splash')).not.toBeNull();
    expect(commands('tone3000List')).toHaveLength(0);
  });

  it('connecting through the splash lists the favorites once the status event arrives', async () => {
    await opened(signedOut);
    button('Connect TONE3000').click();
    juce.reply(last('tone3000Connect').id, true, { connected: true, user });
    event('tone3000Status', connected);
    await answerLists([tone('1')]);
    expect(content.querySelectorAll('li.tone-row')).toHaveLength(1);
  });
});

describe('online module', () => {
  it('licenseLabel', () => {
    expect(
      ['t3k', 'cc-by', 'cc-by-sa', 'cc-by-nc', 'cc-by-nc-sa', 'cc-by-nc-nd', 'cc-by-nd', 'cco', 'weird'].map(online.licenseLabel),
    ).toEqual([
      'TONE3000 license',
      'CC BY — credit the creator',
      'CC BY-SA — credit, share alike',
      'CC BY-NC — credit the creator, non-commercial',
      'CC BY-NC-SA — credit, non-commercial, share alike',
      'CC BY-NC-ND — credit, non-commercial, no derivatives',
      'CC BY-ND — credit, no derivatives',
      'CC0 — public domain',
      'weird',
    ]);
  });

  const job = (state: string, over: object = {}) => ({ jobId: 'j1', modelId: 'm1', toneId: 't1', state, progress: 1, ...over });

  it('toasts a failed job with the title the UI gave the download', async () => {
    await started(connected);
    const p = online.startDownload({ modelId: 'm1', toneId: 't1' }, 'Fillmore');
    juce.reply(last('tone3000Download').id, true, { jobId: 'j1' });
    await p;
    event('tone3000Download', job('failed', { error: 'disk full' }));
    expect(document.getElementById('toasts')!.textContent).toContain("Couldn't download Fillmore: disk full");
    expect(online.jobsFor('m1')).toHaveLength(1);
  });

  it('toasts done, updated and gone, falling back to "the model"', async () => {
    await started(connected);
    event('tone3000Download', job('done'));
    event('tone3000Download', job('done', { jobId: 'j2', updated: true }));
    event('tone3000Download', job('failed', { jobId: 'j3', error: 'gone' }));
    const text = document.getElementById('toasts')!.textContent!;
    expect(text).toContain('the model added to your library');
    expect(text).toContain('the model was updated on TONE3000 since this rig was saved');
    expect(text).toContain('the model is no longer on TONE3000');
  });

  it('a canceled job is not toasted', async () => {
    await started(connected);
    event('tone3000Download', job('downloading', { progress: 0.2 }));
    event('tone3000Download', job('failed', { error: 'canceled' }));
    expect(document.getElementById('toasts')?.textContent ?? '').not.toContain("Couldn't download");
    expect(online.jobsFor('m1').at(-1)!.state).toBe('failed');
  });

  it('does not toast the same state twice and lets onJob listeners unsubscribe', async () => {
    await started(connected);
    const seen: string[] = [];
    const off = online.onJob((j) => seen.push(j.state));
    event('tone3000Download', job('downloading', { progress: 0.2 }));
    event('tone3000Download', job('downloading', { progress: 0.6 }));
    off();
    event('tone3000Download', job('verifying'));
    expect(seen).toEqual(['downloading', 'downloading']);
    expect(document.getElementById('toasts')).toBeNull();
  });

  it('a done job refreshes the library marks', async () => {
    await opened(connected, [tone('1')]);
    const before = commands('listAssets').length;
    event('tone3000Download', job('done'));
    expect(commands('listAssets').length).toBe(before + 2);
  });

  it('getState seeds the status and the tone3000Status event updates it', async () => {
    await started(connected);
    expect(state.store.get().tone3000.user?.username).toBe('ada');
    event('tone3000Status', signedOut);
    expect(state.store.get().tone3000.connected).toBe(false);
  });
});

// ---- Task 17: tone detail ----

const node = (id: string, type: string) => ({ id, type, bypass: false, params: {} });
const chain = (...types: string[]) => types.map((t, i) => node(`n${i}`, t));
const DEFAULT_RIG = { id: 'r', name: 'R', band: 'B', nodes: chain('Input', 'Gate', 'NamAmp', 'CabIR', 'Output'), connections: [], mappings: [] };
const models = [
  { id: 'm1', name: 'Crunch', size: 'standard', architecture: '2' },
  { id: 'm2', name: 'Lead', size: 'lite', architecture: '2' },
  { id: 'm3', name: 'Old', size: 'standard', architecture: '1' },
];
const fullTone = (over: object = {}) => ({
  ...tone('9', { title: 'Plexi Pack', creator: { ...user, username: 'jane' }, license: 'cc-by-nc' }),
  description: 'Line one\nLine two',
  makes: [], tags: [], images: [], publishedAt: '', models,
  ...over,
});
const modal = () => document.querySelector<HTMLElement>('.modal')!;
const mbutton = (text: string) => [...modal().querySelectorAll<HTMLButtonElement>('button')].find((b) => b.textContent === text)!;
const select = () => modal().querySelector<HTMLSelectElement>('select')!;

async function detail(t: object, opts: { rig?: object | null; target?: any; assets?: object[] } = {}) {
  if (commands('getState').length === 0) await started(connected, opts.rig === undefined ? DEFAULT_RIG : opts.rig);
  void view.openToneDetail('9', opts.target);
  await flush();
  juce.reply(last('tone3000Tone').id, true, t);
  await flush();
  for (const c of commands('listAssets').slice(-2)) juce.reply(c.id, true, { assets: c.args.kind === 'nam' ? (opts.assets ?? []) : [] });
  await flush();
}

afterEach(() => {
  document.querySelectorAll('.modal-backdrop').forEach((m) => m.remove());
});

describe('addToRigTarget', () => {
  const t = (format: 'nam' | 'ir', ...types: string[]) => view.addToRigTarget(chain('Input', ...types, 'Output') as any, format);
  it('NAM goes right after the last NamAmp', () => {
    expect(t('nam', 'Gate', 'NamAmp', 'CabIR')).toEqual({ type: 'NamAmp', index: 2 });
    expect(t('nam', 'NamAmp', 'Plugin', 'NamAmp', 'CabIR')).toEqual({ type: 'NamAmp', index: 3 });
  });
  it('NAM without an amp goes before the first CabIR, else at the end', () => {
    expect(t('nam', 'Gate', 'CabIR', 'CabIR')).toEqual({ type: 'NamAmp', index: 1 });
    expect(t('nam', 'Gate', 'Plugin')).toEqual({ type: 'NamAmp', index: 2 });
    expect(t('nam')).toEqual({ type: 'NamAmp', index: 0 });
  });
  it('IR goes right after the last NamAmp or CabIR, else at the end', () => {
    expect(t('ir', 'Gate', 'NamAmp', 'CabIR', 'EQ')).toEqual({ type: 'CabIR', index: 3 });
    expect(t('ir', 'NamAmp', 'EQ')).toEqual({ type: 'CabIR', index: 1 });
    expect(t('ir', 'Gate', 'EQ')).toEqual({ type: 'CabIR', index: 2 });
  });
});

describe('tone detail', () => {
  it('renders the tone: title, creator, license, models (A2 first, first selected)', async () => {
    await detail(fullTone(), { assets: [{ name: 'x', file: '/x', source: { provider: 'tone3000', id: 'm2', toneId: '9' } }] });
    expect(modal().getAttribute('aria-label')).toBe('Plexi Pack');
    expect(modal().textContent).toContain('jane');
    expect(modal().textContent).toContain('CC BY-NC — credit the creator, non-commercial');
    expect([...select().options].map((o) => o.textContent)).toEqual([
      'Crunch · standard · A2',
      'Lead · lite · A2 (in library)',
      'Old · standard · A1',
    ]);
    expect(select().value).toBe('m1');
    expect(modal().querySelector<HTMLElement>('.tone-description')!.textContent).toBe('Line one\nLine two');
  });

  it('omits the architecture part for an IR model', async () => {
    await detail(fullTone({ format: 'ir', models: [{ id: 'i1', name: 'Close', size: '', architecture: '' }] }));
    expect(select().options[0].textContent).toBe('Close');
  });

  it('shows hostile text as text', async () => {
    await detail(fullTone({ title: '<img src=x onerror=alert(1)>', description: '<img src=y onerror=alert(2)>' }));
    expect(modal().querySelector('img[src="x"]')).toBeNull();
    expect(modal().querySelector('img[src="y"]')).toBeNull();
    expect(modal().textContent).toContain('<img src=y onerror=alert(2)>');
  });

  it('View on TONE3000 goes through openExternal, and only for tone3000.com URLs', async () => {
    await detail(fullTone({ url: 'https://www.tone3000.com/tones/9' }));
    mbutton('View on TONE3000').click();
    expect(natives('openExternal').at(-1)!.payload.params).toEqual(['https://www.tone3000.com/tones/9']);
    modal().closest('.modal-backdrop')!.remove();
    await detail(fullTone({ url: 'https://evil.example/x' }));
    expect(mbutton('View on TONE3000')).toBeUndefined();
  });

  it('Download sends the selected model; the toast names tone and model', async () => {
    await detail(fullTone());
    select().value = 'm2';
    select().dispatchEvent(new Event('change'));
    mbutton('Download').click();
    expect(last('tone3000Download').args).toEqual({ modelId: 'm2', toneId: '9' });
    juce.reply(last('tone3000Download').id, true, { jobId: 'j1' });
    await flush();
    event('tone3000Download', { jobId: 'j1', modelId: 'm2', toneId: '9', state: 'done', progress: 1 });
    expect(document.getElementById('toasts')!.textContent).toContain('Plexi Pack — Lead added to your library');
    expect(modal()).not.toBeNull(); // done closes nothing
  });

  it('a job event that beats the reply still toasts the title', async () => {
    await detail(fullTone());
    mbutton('Download').click();
    event('tone3000Download', { jobId: 'j7', modelId: 'm1', toneId: '9', state: 'failed', progress: 0, error: 'disk full' });
    juce.reply(last('tone3000Download').id, true, { jobId: 'j7' });
    await flush();
    expect(document.getElementById('toasts')!.textContent).toContain("Couldn't download Plexi Pack — Crunch: disk full");
  });

  it('Add to rig sends addNode: NamAmp index 2 for a NAM tone, CabIR index 3 for an IR tone', async () => {
    await detail(fullTone());
    mbutton('Add to rig').click();
    expect(last('tone3000Download').args).toEqual({ modelId: 'm1', toneId: '9', addNode: { type: 'NamAmp', index: 2 } });
    modal().closest('.modal-backdrop')!.remove();
    await detail(fullTone({ format: 'ir', models: [{ id: 'i1', name: 'Close', size: '', architecture: '' }] }));
    mbutton('Add to rig').click();
    expect(last('tone3000Download').args).toEqual({ modelId: 'i1', toneId: '9', addNode: { type: 'CabIR', index: 3 } });
  });

  it('Add to rig is disabled without a rig', async () => {
    await detail(fullTone(), { rig: null });
    expect(mbutton('Add to rig').disabled).toBe(true);
    expect(mbutton('Download').disabled).toBe(false);
  });

  it('Use on <node> replaces the node, next to Download', async () => {
    await detail(fullTone(), { target: { nodeId: 'n2' } });
    expect(mbutton('Add to rig')).toBeUndefined();
    mbutton('Use on NAM Amp').click();
    expect(last('tone3000Download').args).toEqual({ modelId: 'm1', toneId: '9', nodeId: 'n2' });
    expect(mbutton('Download')).toBeDefined();
  });

  it('a tone without models has no actions', async () => {
    await detail(fullTone({ models: [] }));
    expect(modal().textContent).toContain('This tone has no downloadable models.');
    expect(mbutton('Download')).toBeUndefined();
  });

  it('other formats cannot be loaded', async () => {
    await detail(fullTone({ format: 'aida-x' }));
    expect(modal().textContent).toContain("AMPerator can't load this format.");
    expect(mbutton('Download').disabled).toBe(true);
    expect(mbutton('Add to rig').disabled).toBe(true);
  });

  it('shows progress, disables the actions, cancels, and shows a failure inline', async () => {
    await detail(fullTone());
    mbutton('Download').click();
    juce.reply(last('tone3000Download').id, true, { jobId: 'j1' });
    await flush();
    event('tone3000Download', { jobId: 'j1', modelId: 'm1', toneId: '9', state: 'downloading', progress: 0.5 });
    expect(modal().querySelector<HTMLProgressElement>('progress')!.value).toBe(0.5);
    expect(mbutton('Download').disabled).toBe(true);
    mbutton('Cancel').click();
    expect(last('tone3000Cancel').args).toEqual({ jobId: 'j1' });
    event('tone3000Download', { jobId: 'j1', modelId: 'm1', toneId: '9', state: 'failed', progress: 0.5, error: 'cancelled' });
    expect(modal().querySelector('.tone-job-error')!.textContent).toContain('cancelled');
    expect(mbutton('Download').disabled).toBe(false);
    expect(modal().querySelector('progress')).toBeNull();
  });

  it('a double click starts one download; the actions stay disabled until the first job event', async () => {
    await detail(fullTone());
    mbutton('Add to rig').click();
    mbutton('Add to rig').click();
    mbutton('Download').click();
    expect(commands('tone3000Download')).toHaveLength(1);
    expect(mbutton('Add to rig').disabled).toBe(true);
    expect(mbutton('Download').disabled).toBe(true);
    juce.reply(last('tone3000Download').id, true, { jobId: 'j1' });
    await flush();
    expect(mbutton('Add to rig').disabled).toBe(true); // the reply alone does not re-enable
    event('tone3000Download', { jobId: 'j1', modelId: 'm1', toneId: '9', state: 'queued', progress: 0 });
    expect(mbutton('Add to rig').disabled).toBe(true); // running now
    event('tone3000Download', { jobId: 'j1', modelId: 'm1', toneId: '9', state: 'done', progress: 1 });
    expect(mbutton('Add to rig').disabled).toBe(false);
    expect(commands('tone3000Download')).toHaveLength(1);
  });

  it('a start that fails re-enables the actions', async () => {
    await detail(fullTone());
    mbutton('Download').click();
    expect(mbutton('Download').disabled).toBe(true);
    juce.reply(last('tone3000Download').id, false, 'not connected');
    await flush();
    expect(mbutton('Download').disabled).toBe(false);
    expect(document.getElementById('toasts')!.textContent).toContain('not connected');
  });

  it('links the creator only with the URL TONE3000 gave, through openExternal', async () => {
    await detail(fullTone());
    mbutton('View creator on TONE3000').click();
    expect(natives('openExternal').at(-1)!.payload.params).toEqual(['https://www.tone3000.com/ada']);
    modal().closest('.modal-backdrop')!.remove();
    await detail(fullTone({ creator: { ...user, username: 'jane', url: '' } }));
    expect(mbutton('View creator on TONE3000')).toBeUndefined();
    expect(mbutton('View on TONE3000')).toBeDefined(); // the tone link stays
    modal().closest('.modal-backdrop')!.remove();
    await detail(fullTone({ creator: { ...user, url: 'https://evil.example/ada' } }));
    expect(mbutton('View creator on TONE3000')).toBeUndefined();
  });

  it('shows the progress of an already running job for the selected model', async () => {
    await started(connected, DEFAULT_RIG);
    event('tone3000Download', { jobId: 'j1', modelId: 'm1', toneId: '9', state: 'downloading', progress: 0.25 });
    await detail(fullTone());
    expect(modal().querySelector<HTMLProgressElement>('progress')!.value).toBe(0.25);
  });

  it('closing the modal drops its subscriptions', async () => {
    await detail(fullTone());
    const body = modal();
    document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape' }));
    event('tone3000Download', { jobId: 'j1', modelId: 'm1', toneId: '9', state: 'downloading', progress: 0.9 });
    expect(body.querySelector('progress')).toBeNull();
  });
});

describe('registry and list robustness', () => {
  it('prunes finished jobs beyond the last 50', async () => {
    await started(connected);
    for (let i = 0; i < 60; i++) event('tone3000Download', { jobId: `j${i}`, modelId: `m${i}`, toneId: 't', state: 'done', progress: 1 });
    expect(online.jobsFor('m0')).toEqual([]);
    expect(online.jobsFor('m59')).toHaveLength(1);
    event('tone3000Download', { jobId: 'live', modelId: 'mlive', toneId: 't', state: 'downloading', progress: 0.1 });
    for (let i = 60; i < 120; i++) event('tone3000Download', { jobId: `j${i}`, modelId: `m${i}`, toneId: 't', state: 'done', progress: 1 });
    expect(online.jobsFor('mlive')).toHaveLength(1); // running jobs are never pruned
  });

  it('drops a list reply that arrives after another sub-tab was chosen', async () => {
    await opened(connected);
    button('Trending').click();
    const trending = last('tone3000List').id;
    button('Latest').click();
    juce.reply(last('tone3000List').id, true, { tones: [tone('L')], page: 1, totalPages: 1 });
    await flush();
    juce.reply(trending, true, { tones: [tone('T')], page: 1, totalPages: 1 });
    await flush();
    expect(content.querySelector('.tone-title')!.textContent).toBe('Tone L');
  });

  it('dispose unsubscribes onJob', async () => {
    await opened(connected);
    dispose();
    const before = commands('listAssets').length;
    event('tone3000Download', { jobId: 'j', modelId: 'm', toneId: 't', state: 'done', progress: 1 });
    expect(commands('listAssets').length).toBe(before);
  });
});
