import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { installFakeJuce, type FakeJuce } from '../test-support';

let juce: FakeJuce;
let state: typeof import('../state');
let library: typeof import('./library');
let root: HTMLElement;
let removeShortcuts: (() => void) | null = null;

const emptyReport = { missingAssets: [], missingPlugins: [], placeholderNodeIds: [], resolvedByFilename: [] };
const rig = { id: 'dog', name: 'Black Dog', band: 'Zeppelin', tags: ['70s'], nodes: [], connections: [], mappings: [] };

const RIGS = [
  { id: 'djent', name: 'Peavey 5150 - Djent', band: 'Metal', tags: ['Parkway Drive', 'Wild Eyes', 'metal'], favorite: true },
  { id: 'plush', name: 'Plush', band: 'STP', tags: ['clean', '90s'], favorite: false },
  { id: 'dog', name: 'Black Dog', band: 'Zeppelin', tags: ['70s'], favorite: false },
  { id: 'kash', name: 'Kashmir', band: 'Zeppelin', tags: ['70s', 'clean'], favorite: false },
];
const TAGS = [
  { tag: '70s', count: 2 },
  { tag: 'clean', count: 2 },
  { tag: '90s', count: 1 },
  { tag: 'metal', count: 1 },
  { tag: 'Parkway Drive', count: 1 },
  { tag: 'Wild Eyes', count: 1 },
];

const flush = () => new Promise((r) => setTimeout(r, 0));
const pending = (cmd: string) => juce.commands().filter((c) => c.cmd === cmd).at(-1)!;
const search = () => root.querySelector<HTMLInputElement>('.library-search')!;
const chips = () => [...root.querySelectorAll<HTMLButtonElement>('.tag-filter')];
const chip = (tag: string) => chips().find((c) => c.querySelector('.tag-filter-name')!.textContent === tag)!;
const clearLink = () => root.querySelector<HTMLButtonElement>('.tag-clear');
const bandGroups = () =>
  [...root.querySelectorAll('.band-group')].map((g) => [
    g.querySelector('.band-name')!.textContent,
    ...[...g.querySelectorAll('.rig-load')].map((b) => b.textContent),
  ]);
const sectionTitles = () => [...root.querySelectorAll('.library-section h3')].map((h) => h.textContent);
function typeSearch(text: string) {
  search().value = text;
  search().dispatchEvent(new Event('input', { bubbles: true }));
}
const key = (k: string, opts: KeyboardEventInit = {}, target: EventTarget = document.body) =>
  target.dispatchEvent(new KeyboardEvent('keydown', { key: k, bubbles: true, cancelable: true, ...opts }));

async function started(extra: object = {}) {
  const p = state.initStore();
  juce.reply(pending('getState').id, true, {
    protocol: 1, rig, report: emptyReport, dirty: false, saved: true, learning: false, startupErrors: [], settings: {},
    deviceStatus: { status: 'Running', latencyMs: 8, xruns: 0 }, controlStatus: { hid: 'NotFound', exclusive: false, midiDevices: [] },
    tunerOn: false, favorites: ['djent'], recents: ['kash'], bands: ['Metal', 'STP', 'Zeppelin'],
    ...extra,
  });
  await p;
}

/** Renders the library and answers its listRigs / listTags. */
async function rendered(rigs = RIGS, tags = TAGS) {
  await started();
  library.renderLibrary(root);
  juce.reply(pending('listRigs').id, true, { rigs, bands: [...new Set(rigs.map((r) => r.band))] });
  juce.reply(pending('listTags').id, true, { tags });
  await flush();
}

beforeEach(async () => {
  vi.resetModules();
  document.body.innerHTML = '';
  juce = installFakeJuce();
  state = await import('../state');
  library = await import('./library');
  root = document.createElement('aside');
  document.body.append(root);
});

afterEach(() => {
  removeShortcuts?.();
  removeShortcuts = null;
  delete (window as any).__JUCE__;
});

describe('library search and tag filter bar', () => {
  it('shows a search field and a chip per tag with its count, in listTags order', async () => {
    await rendered();
    expect(search().placeholder).toBe('Search rigs, bands, tags…');
    expect(chips().map((c) => c.textContent)).toEqual(['70s2', 'clean2', '90s1', 'metal1', 'Parkway Drive1', 'Wild Eyes1']);
    expect(chips().map((c) => c.querySelector('.tag-filter-count')!.textContent)).toEqual(['2', '2', '1', '1', '1', '1']);
    expect(chips().every((c) => c.getAttribute('aria-pressed') === 'false')).toBe(true);
    expect(clearLink()).toBeNull();
    // the search sits above the rig list
    expect(search().compareDocumentPosition(root.querySelector('.band-group')!) & Node.DOCUMENT_POSITION_FOLLOWING).toBeTruthy();
  });

  it('rows show the rig tags', async () => {
    await rendered();
    const row = [...root.querySelectorAll('.band-group .rig-row')].find((r) => r.querySelector('.rig-load')!.textContent === 'Kashmir')!;
    expect(row.querySelector('.rig-tags')!.textContent).toBe('70s · clean');
  });

  it('typing filters the rigs live by name, band or tag and hides empty bands', async () => {
    await rendered();
    expect(bandGroups()).toEqual([['Metal', 'Peavey 5150 - Djent'], ['STP', 'Plush'], ['Zeppelin', 'Black Dog', 'Kashmir']]);
    expect(sectionTitles()).toEqual(['Favorites', 'Recent', 'Bands']);

    typeSearch('parkway');
    expect(bandGroups()).toEqual([['Metal', 'Peavey 5150 - Djent']]);
    expect(sectionTitles()).toEqual(['Bands']); // favorites / recent are not repeated while filtering

    typeSearch('WILD eyes');
    expect(bandGroups()).toEqual([['Metal', 'Peavey 5150 - Djent']]);
    typeSearch('zeppelin clean');
    expect(bandGroups()).toEqual([['Zeppelin', 'Kashmir']]);
    typeSearch('plu');
    expect(bandGroups()).toEqual([['STP', 'Plush']]);

    typeSearch('nothing like this');
    expect(bandGroups()).toEqual([]);
    expect(root.querySelector('.library-content')!.textContent).toContain('No rigs match');

    typeSearch('');
    expect(bandGroups()).toHaveLength(3);
    expect(sectionTitles()).toEqual(['Favorites', 'Recent', 'Bands']);
  });

  it('the search field keeps the focus while the list re-renders', async () => {
    await rendered();
    search().focus();
    typeSearch('70s');
    juce.fromBackend('amp', { v: 1, event: 'tagsChanged', data: { tags: ['70s', 'blues'] } }); // refetches the list
    juce.reply(pending('listRigs').id, true, { rigs: RIGS, bands: [] });
    await flush();
    expect(document.activeElement).toBe(search());
    expect(search().value).toBe('70s');
    expect(bandGroups()).toEqual([['Zeppelin', 'Black Dog', 'Kashmir']]);
  });

  it('chips toggle; several selected tags must all be on the rig; Clear resets them', async () => {
    await rendered();
    chip('70s').click();
    expect(chip('70s').getAttribute('aria-pressed')).toBe('true');
    expect(bandGroups()).toEqual([['Zeppelin', 'Black Dog', 'Kashmir']]);
    expect(clearLink()).not.toBeNull();

    chip('clean').click();
    expect(bandGroups()).toEqual([['Zeppelin', 'Kashmir']]);

    chip('metal').click();
    expect(bandGroups()).toEqual([]);
    expect(root.textContent).toContain('No rigs match');

    chip('metal').click(); // toggles off
    expect(bandGroups()).toEqual([['Zeppelin', 'Kashmir']]);

    typeSearch('plush');
    expect(bandGroups()).toEqual([]); // query and tags combine

    typeSearch('');
    clearLink()!.click();
    expect(chips().every((c) => c.getAttribute('aria-pressed') === 'false')).toBe(true);
    expect(clearLink()).toBeNull();
    expect(bandGroups()).toHaveLength(3);
  });

  it('refreshes the tags after a save, a delete and a tag change', async () => {
    await rendered();
    const listTagsCalls = () => juce.commands().filter((c) => c.cmd === 'listTags').length;
    expect(listTagsCalls()).toBe(1);

    state.rigSaved('Black Dog', 'Zeppelin'); // same name/band: still refetched
    await flush();
    expect(listTagsCalls()).toBe(2);
    juce.reply(pending('listTags').id, true, { tags: [{ tag: 'blues', count: 1 }, ...TAGS] });
    await flush();
    expect(chips()[0].textContent).toBe('blues1');

    juce.fromBackend('amp', { v: 1, event: 'tagsChanged', data: { tags: ['70s', 'live'] } });
    await flush();
    expect(listTagsCalls()).toBe(3);

    const row = [...root.querySelectorAll('.band-group .rig-row')].find((r) => r.querySelector('.rig-load')!.textContent === 'Plush')!;
    row.querySelector<HTMLButtonElement>('.rig-delete')!.click();
    [...document.querySelectorAll<HTMLButtonElement>('.modal button')].find((b) => b.textContent === 'Move to Trash')!.click();
    await flush();
    juce.reply(pending('deleteRig').id, true, {});
    await flush();
    expect(listTagsCalls()).toBe(4);
  });

  it('drops a selected tag that no rig carries any more', async () => {
    await rendered();
    chip('90s').click();
    expect(bandGroups()).toEqual([['STP', 'Plush']]);
    state.rigSaved('Black Dog', 'Zeppelin');
    await flush();
    juce.reply(pending('listRigs').id, true, { rigs: RIGS.filter((r) => r.id !== 'plush'), bands: [] });
    juce.reply(pending('listTags').id, true, { tags: TAGS.filter((t) => t.tag !== '90s') });
    await flush();
    expect(clearLink()).toBeNull();
    expect(bandGroups()).toHaveLength(2);
  });

  it('a stale listRigs / listTags reply never overwrites a newer one', async () => {
    await rendered();
    state.rigSaved('Black Dog', 'Zeppelin'); // refresh #2
    await flush();
    const rigs2 = pending('listRigs');
    const tags2 = pending('listTags');
    juce.fromBackend('amp', { v: 1, event: 'tagsChanged', data: { tags: ['blues'] } }); // refresh #3
    await flush();
    const rigs3 = pending('listRigs');
    const tags3 = pending('listTags');
    expect(rigs3.id).not.toBe(rigs2.id);

    juce.reply(rigs3.id, true, { rigs: RIGS.filter((r) => r.id === 'plush'), bands: [] });
    juce.reply(tags3.id, true, { tags: [{ tag: 'new', count: 1 }] });
    await flush();
    juce.reply(rigs2.id, true, { rigs: RIGS, bands: [] }); // older answers arrive last
    juce.reply(tags2.id, true, { tags: TAGS });
    await flush();
    expect(bandGroups()).toEqual([['STP', 'Plush']]);
    expect(chips().map((c) => c.textContent)).toEqual(['new1']);
  });

  it('works with rigs listed without tags', async () => {
    await rendered(
      RIGS.map(({ tags: _tags, ...r }) => r) as any,
      [],
    );
    expect(chips()).toEqual([]);
    typeSearch('kash');
    expect(bandGroups()).toEqual([['Zeppelin', 'Kashmir']]);
  });
});

describe('library shortcuts', () => {
  it('⌘F opens the library and focuses the search field', async () => {
    await rendered();
    removeShortcuts = library.installLibraryShortcuts();
    expect(root.classList.contains('open')).toBe(false);
    const ev = new KeyboardEvent('keydown', { key: 'f', metaKey: true, bubbles: true, cancelable: true });
    document.body.dispatchEvent(ev);
    expect(ev.defaultPrevented).toBe(true);
    expect(root.classList.contains('open')).toBe(true);
    expect(document.activeElement).toBe(search());
  });

  it('⌘F works from another library tab and while typing elsewhere', async () => {
    await rendered();
    removeShortcuts = library.installLibraryShortcuts();
    library.toggleLibrary(true);
    [...root.querySelectorAll<HTMLButtonElement>('[role="tab"]')].find((t) => t.textContent === 'Models')!.click();
    const other = document.createElement('input');
    document.body.append(other);
    other.focus();
    key('f', { ctrlKey: true }, other);
    expect(root.querySelector('[role="tab"][aria-selected="true"]')!.textContent).toBe('Rigs');
    expect(document.activeElement).toBe(search());
  });

  it('/ focuses the search field when no text field has the focus', async () => {
    await rendered();
    removeShortcuts = library.installLibraryShortcuts();
    key('/');
    expect(root.classList.contains('open')).toBe(true);
    expect(document.activeElement).toBe(search());

    search().blur();
    library.toggleLibrary(false);
    const other = document.createElement('input');
    document.body.append(other);
    other.focus();
    const ev = new KeyboardEvent('keydown', { key: '/', bubbles: true, cancelable: true });
    other.dispatchEvent(ev);
    expect(ev.defaultPrevented).toBe(false); // typing a slash stays a slash
    expect(root.classList.contains('open')).toBe(false);
  });

  it('Escape in the search field clears the query', async () => {
    await rendered();
    typeSearch('plush');
    key('Escape', {}, search());
    expect(search().value).toBe('');
    expect(bandGroups()).toHaveLength(3);
  });
});

describe('Models / IRs attribution and the TONE3000 tab', () => {
  const tabButton = (label: string) => [...root.querySelectorAll<HTMLButtonElement>('[role="tab"]')].find((t) => t.textContent === label)!;
  const source = {
    provider: 'tone3000', id: '7', toneId: '3', title: 'Plexi', license: 'cc-by-nc',
    creator: { username: 'ada', displayName: 'Ada', avatarUrl: '', url: '' }, url: '', image: '',
  };

  it('shows creator and license under a TONE3000 model, and nothing under a plain file', async () => {
    await rendered();
    tabButton('Models').click();
    juce.reply(pending('listAssets').id, true, {
      assets: [
        { name: 'Plexi', file: '/m/TONE3000/ada/Plexi.nam', source },
        { name: 'Mine', file: '/m/Mine.nam' },
      ],
    });
    await flush();
    const rows = [...root.querySelectorAll('.library-list li')];
    expect(rows[0].querySelector('.attribution')!.textContent).toBe('ada · CC BY-NC — credit the creator, non-commercial');
    expect(rows[1].querySelector('.attribution')).toBeNull();
  });

  it('the TONE3000 tab is a logo button that renders the tab and cleans it up when another tab is shown', async () => {
    await rendered();
    const brand = root.querySelector<HTMLButtonElement>('[role="tab"][aria-label="TONE3000"]')!;
    expect(brand.querySelector('img')!.alt).toBe('TONE3000');
    juce.fromBackend('amp', { v: 1, event: 'tone3000Status', data: { configured: true, connected: false, busy: false, remembered: false, user: null } });
    brand.click();
    expect(root.querySelector('.tone-splash')).not.toBeNull(); // not connected: the splash
    tabButton('Models').click();
    expect(root.querySelector('.tone-splash')).toBeNull();
    // the tab's store subscription is gone: a later status change does not redraw it
    juce.fromBackend('amp', { v: 1, event: 'tone3000Status', data: { configured: true, connected: true, busy: false, remembered: false, user: null } });
    expect(root.querySelector('.tone-account')).toBeNull();
    expect(juce.commands().filter((c) => c.cmd === 'tone3000List')).toHaveLength(0);
  });

  it('closing the panel disposes the TONE3000 tab; reopening shows it again', async () => {
    await rendered();
    const status = (connected: boolean) =>
      juce.fromBackend('amp', { v: 1, event: 'tone3000Status', data: { configured: true, connected, busy: false, remembered: false, user: null } });
    status(false);
    root.querySelector<HTMLButtonElement>('[role="tab"][aria-label="TONE3000"]')!.click();
    library.toggleLibrary(true);
    library.toggleLibrary(false);
    status(true); // while closed nothing is drawn or requested
    expect(juce.commands().filter((c) => c.cmd === 'tone3000List')).toHaveLength(0);
    library.toggleLibrary(true);
    expect(root.querySelector('.tone-account, .tone-results')).not.toBeNull(); // re-rendered on reopen
  });

  it('rendering the library again disposes the previous TONE3000 tab', async () => {
    await rendered();
    juce.fromBackend('amp', { v: 1, event: 'tone3000Status', data: { configured: true, connected: false, busy: false, remembered: false, user: null } });
    root.querySelector<HTMLButtonElement>('[role="tab"][aria-label="TONE3000"]')!.click();
    const old = root.querySelector('.library-content')!;
    library.renderLibrary(document.createElement('aside'));
    juce.fromBackend('amp', { v: 1, event: 'tone3000Status', data: { configured: true, connected: true, busy: false, remembered: false, user: null } });
    expect(old.querySelector('.tone-account')).toBeNull();
  });
});
