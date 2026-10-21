// Library panel (slides out from the left in both views): a search field and a tag filter bar over bands → rigs,
// favorites, recents, New rig, and read-only Models / IRs / Plugins lists. "Search online" is a placeholder tab
// (sub-project 3). ⌘F (and / when no text field has the focus) opens the panel on the search field.

import { call } from '../bridge';
import { confirmDialog, el, toast } from '../dialog';
import { isTyping } from '../keys';
import { filterRigs } from '../rigFilter';
import { confirmDiscardIfDirty, setView, store, type AppState } from '../state';

interface RigEntry {
  id: string;
  name: string;
  band: string;
  tags?: string[];
  favorite: boolean;
}

interface TagCount {
  tag: string;
  count: number;
}

type Tab = 'rigs' | 'models' | 'irs' | 'plugins';

const MAX_RECENTS = 10; // Settings::maxRecent

let panel: HTMLElement | null = null;
let onToggle: ((open: boolean) => void) | null = null;
let focusSearch: (() => void) | null = null;

/** Opens or closes the panel rendered by renderLibrary. Returns the new state. */
export function toggleLibrary(open = !panel?.classList.contains('open')): boolean {
  if (!panel) return false;
  panel.classList.toggle('open', open);
  panel.toggleAttribute('inert', !open);
  panel.setAttribute('aria-hidden', String(!open));
  onToggle?.(open);
  return open;
}

/** Opens the panel on the Rigs tab with the focus in its search field. */
export function focusLibrarySearch(): void {
  focusSearch?.();
}

/** ⌘F / Ctrl+F (anywhere) and / (when no text field has the focus) open the library on its search field. Returns a
    function that removes the shortcuts. */
export function installLibraryShortcuts(): () => void {
  const onKey = (e: KeyboardEvent) => {
    if (e.defaultPrevented || e.repeat || e.altKey) return;
    const find = (e.metaKey || e.ctrlKey) && !e.shiftKey && e.key.toLowerCase() === 'f';
    const slash = e.key === '/' && !e.metaKey && !e.ctrlKey && !isTyping(e.target);
    if (!find && !slash) return;
    if (store.get().learning || document.querySelector('.modal-backdrop')) return;
    e.preventDefault();
    focusLibrarySearch();
  };
  document.addEventListener('keydown', onKey);
  return () => document.removeEventListener('keydown', onKey);
}

/** `fn` runs whenever the panel opens or closes (the header's library button reflects it). */
export function onLibraryToggle(fn: (open: boolean) => void) {
  onToggle = fn;
}

/** Loads a rig after confirming that unsaved changes may be discarded; a failure offers the last backup. */
export async function openRig(id: string, name = id): Promise<void> {
  if (!(await confirmDiscardIfDirty())) return;
  try {
    await call('loadRig', { id });
  } catch (e) {
    toast(`Couldn't open “${name}”: ${(e as Error).message}`, 'error', 12000, { label: 'Open last backup', run: () => void restore(id) });
  }
}

async function restore(id: string) {
  if (!(await confirmDiscardIfDirty())) return;
  try {
    await call('restoreBackup', { id });
  } catch (e) {
    toast((e as Error).message);
  }
}

/** What the library refetches on: the current rig's identity, name, band, tags and saved state, and every save. */
function libraryKey(s: AppState): string {
  const rig = s.rig;
  return JSON.stringify([rig?.id, rig?.name, rig?.band, rig?.tags ?? [], s.saved, s.saveCount]);
}

export function renderLibrary(root: HTMLElement): void {
  panel = root;
  root.classList.add('library');
  root.setAttribute('aria-label', 'Library');

  let tab: Tab = 'rigs';
  let rigs: RigEntry[] = [];
  let tagCounts: TagCount[] = [];
  let query = '';
  let selectedTags: string[] = []; // as spelled in tagCounts
  let recents = [...store.get().recents];
  let lastKey = '';
  let lastSavedRig = store.get().saved ? (store.get().rig?.id ?? '') : ''; // id of the current rig once saved

  const close = el('button', { class: 'icon-button', 'aria-label': 'Close library', title: 'Close' }, '×');
  close.addEventListener('click', () => toggleLibrary(false));
  const tabs = el('div', { class: 'library-tabs', role: 'tablist' });
  const content = el('div', { class: 'library-content', role: 'tabpanel' });
  root.replaceChildren(el('header', { class: 'library-header' }, el('h2', {}, 'Library'), close), tabs, content);

  // Rigs tab: the search field and the tag bar stay in the DOM while the list below re-renders (keeps the focus).
  const search = el('input', {
    class: 'library-search',
    type: 'search',
    placeholder: 'Search rigs, bands, tags…',
    'aria-label': 'Search rigs, bands, tags',
    spellcheck: 'false',
    autocomplete: 'off',
  });
  search.addEventListener('input', () => {
    query = search.value;
    renderRigList();
  });
  search.addEventListener('keydown', (e) => {
    if (e.key !== 'Escape') return;
    e.preventDefault();
    e.stopPropagation();
    if (search.value) {
      search.value = query = '';
      renderRigList();
    } else search.blur();
  });
  const tagBar = el('div', { class: 'tag-filter-bar', role: 'group', 'aria-label': 'Filter by tag' });
  const rigsTop = el('div', { class: 'library-filter' }, search, tagBar);
  const rigsList = el('div', { class: 'library-rigs' });

  focusSearch = () => {
    toggleLibrary(true);
    if (tab !== 'rigs') {
      tab = 'rigs';
      renderTabs();
      renderRigs();
      void refreshRigs();
    }
    search.focus();
    search.select();
  };

  function renderTabs() {
    const def: [Tab | 'online', string][] = [
      ['rigs', 'Rigs'],
      ['models', 'Models'],
      ['irs', 'IRs'],
      ['plugins', 'Plugins'],
      ['online', 'Search online'],
    ];
    tabs.replaceChildren(
      ...def.map(([id, label]) => {
        const b = el('button', { class: 'library-tab', role: 'tab', 'aria-selected': String(id === tab) }, label);
        if (id === 'online') {
          b.disabled = true;
          b.title = 'Search online — coming soon';
          b.append(el('span', { class: 'soon' }, 'coming soon'));
        } else {
          b.addEventListener('click', () => {
            tab = id;
            renderTabs();
            void showTab();
          });
        }
        return b;
      }),
    );
  }

  async function showTab() {
    if (tab === 'rigs') {
      renderRigs(); // the last known list at once
      return refreshRigs();
    }
    content.replaceChildren(el('p', { class: 'muted' }, 'Loading…'));
    const shown = tab;
    try {
      let items: { name: string; detail: string }[];
      if (tab === 'plugins') {
        const r = await call<{ plugins: { name: string; vendor: string; format: string }[] }>('listPlugins');
        items = r.plugins.map((p) => ({ name: p.name, detail: `${p.vendor} · ${p.format}` }));
      } else {
        const r = await call<{ assets: { name: string; file: string }[] }>('listAssets', { kind: tab === 'models' ? 'nam' : 'ir' });
        items = r.assets.map((a) => ({ name: a.name, detail: a.file }));
      }
      if (tab !== shown) return;
      const empty = {
        models: 'No models. Put .nam files in ~/Music/AMPerator/Models or add a watched folder in Settings.',
        irs: 'No IRs. Put .wav files in ~/Music/AMPerator/IRs or add a watched folder in Settings.',
        plugins: 'No plugins. Use “Plugins…” on the pedalboard’s + card to scan.',
      }[shown as 'models' | 'irs' | 'plugins'];
      content.replaceChildren(
        items.length === 0
          ? el('p', { class: 'muted' }, empty)
          : el(
              'ul',
              { class: 'library-list' },
              ...items.map((i) => el('li', { title: i.detail }, el('span', {}, i.name), el('span', { class: 'muted path' }, i.detail))),
            ),
      );
    } catch (e) {
      if (tab === shown) content.replaceChildren(el('p', { class: 'muted' }, (e as Error).message));
    }
  }

  // Each refresh numbers its request: a reply to an older one (answered late) never overwrites a newer result.
  let rigsRequest = 0;
  let tagsRequest = 0;

  async function refreshRigs() {
    void refreshTags(); // answered on its own: the list never waits for the tags
    const request = ++rigsRequest;
    try {
      const r = await call<{ rigs: RigEntry[]; bands: string[] }>('listRigs');
      if (request !== rigsRequest) return;
      rigs = r.rigs;
    } catch (e) {
      if (request !== rigsRequest) return;
      toast(`Couldn't list rigs: ${(e as Error).message}`);
    }
    if (tab === 'rigs') renderRigs();
  }

  async function refreshTags() {
    const request = ++tagsRequest;
    try {
      const r = await call<{ tags: TagCount[] }>('listTags');
      if (request !== tagsRequest) return;
      tagCounts = r.tags;
    } catch {
      return; // the bar keeps the last known tags
    }
    // A selected tag no saved rig carries any more would hide every rig.
    const known = new Set(tagCounts.map((t) => t.tag.toLowerCase()));
    selectedTags = selectedTags.filter((t) => known.has(t.toLowerCase()));
    if (tab === 'rigs') renderRigs();
  }

  function toggleTag(tag: string) {
    const lower = tag.toLowerCase();
    selectedTags = selectedTags.some((t) => t.toLowerCase() === lower)
      ? selectedTags.filter((t) => t.toLowerCase() !== lower)
      : [...selectedTags, tag];
    renderRigs();
  }

  function renderTagBar() {
    const isSelected = (tag: string) => selectedTags.some((t) => t.toLowerCase() === tag.toLowerCase());
    const chips = tagCounts.map((t) => {
      const b = el(
        'button',
        { class: 'tag-filter', type: 'button', 'aria-pressed': String(isSelected(t.tag)), title: `${t.tag}: ${t.count} rig${t.count === 1 ? '' : 's'}` },
        el('span', { class: 'tag-filter-name' }, t.tag),
        el('span', { class: 'tag-filter-count' }, String(t.count)),
      );
      b.addEventListener('click', () => toggleTag(t.tag));
      return b;
    });
    if (selectedTags.length > 0) {
      const clear = el('button', { class: 'tag-clear link-button', type: 'button', title: 'Show rigs with any tags' }, 'Clear');
      clear.addEventListener('click', () => {
        selectedTags = [];
        renderRigs();
      });
      chips.push(clear);
    }
    tagBar.replaceChildren(...chips);
    tagBar.hidden = chips.length === 0;
  }

  function rigRow(r: RigEntry, showBand: boolean) {
    const current = store.get().rig?.id === r.id;
    const load = el('button', { class: 'rig-load', title: `Open ${r.name}` }, r.name);
    if (current) load.setAttribute('aria-current', 'true');
    const tags = r.tags ?? [];
    const main = el('div', { class: 'rig-main' }, load);
    if (tags.length > 0) main.append(el('span', { class: 'muted rig-tags', title: tags.join(', ') }, tags.join(' · ')));
    load.addEventListener('click', () => void openRig(r.id, r.name));
    const star = el(
      'button',
      { class: 'star', 'aria-pressed': String(r.favorite), 'aria-label': `Favorite ${r.name}`, title: r.favorite ? 'Remove from favorites' : 'Add to favorites' },
      r.favorite ? '★' : '☆',
    );
    star.addEventListener('click', async () => {
      try {
        const res = await call<{ id: string; favorite: boolean }>('toggleFavorite', { id: r.id });
        rigs = rigs.map((x) => (x.id === res.id ? { ...x, favorite: res.favorite } : x));
        renderRigs();
      } catch (e) {
        toast((e as Error).message);
      }
    });
    const del = el('button', { class: 'icon-button rig-delete', 'aria-label': `Delete ${r.name}`, title: 'Delete (moves the file to the Trash)' }, 'Delete');
    del.addEventListener('click', async () => {
      if (!(await confirmDialog(`Delete “${r.name}”? The rig file and its backups go to the Trash.`, 'Move to Trash'))) return;
      try {
        await call('deleteRig', { id: r.id });
        await refreshRigs();
      } catch (e) {
        toast((e as Error).message);
      }
    });
    return el('li', { class: `rig-row${current ? ' current' : ''}` }, main, showBand ? el('span', { class: 'muted rig-band' }, r.band) : '', star, del);
  }

  /** The whole Rigs tab: filter bar (kept, so the search field keeps the focus) and the list. */
  function renderRigs() {
    if (rigsTop.parentElement !== content) content.replaceChildren(rigsTop, rigsList);
    renderTagBar();
    renderRigList();
  }

  function renderRigList() {
    const newRig = el('button', { class: 'button primary new-rig' }, 'New rig');
    newRig.addEventListener('click', async () => {
      if (!(await confirmDiscardIfDirty())) return;
      try {
        await call('newRig', { band: store.get().rig?.band ?? 'My Tones' });
        setView('edit');
      } catch (e) {
        toast((e as Error).message);
      }
    });

    // While searching or filtering only the matching rigs are listed, under their bands (favorites and recents
    // would repeat them).
    const filtering = query.trim() !== '' || selectedTags.length > 0;
    const shown = filtering ? filterRigs(rigs, query, selectedTags) : rigs;
    const byId = new Map(rigs.map((r) => [r.id, r]));
    const favorites = filtering ? [] : rigs.filter((r) => r.favorite);
    const recent = filtering ? [] : recents.map((id) => byId.get(id)).filter((r): r is RigEntry => !!r);
    const bands: [string, RigEntry[]][] = [];
    for (const r of shown) {
      const last = bands.at(-1);
      if (last && last[0] === r.band) last[1].push(r);
      else bands.push([r.band, [r]]);
    }

    const section = (title: string, cls: string, rows: HTMLElement[]) =>
      el('section', { class: `library-section ${cls}` }, el('h3', {}, title), el('ul', { class: 'rig-list' }, ...rows));

    rigsList.replaceChildren(
      newRig,
      ...(favorites.length ? [section('Favorites', 'rig-shortcuts', favorites.map((r) => rigRow(r, true)))] : []),
      ...(recent.length ? [section('Recent', 'rig-shortcuts', recent.map((r) => rigRow(r, true)))] : []),
      el(
        'section',
        { class: 'library-section' },
        el('h3', {}, 'Bands'),
        ...(bands.length === 0
          ? [el('p', { class: 'muted' }, filtering ? 'No rigs match' : 'No saved rigs yet. Build one on the pedalboard and press Save.')]
          : bands.map(([band, list]) =>
              el('div', { class: 'band-group' }, el('h4', { class: 'band-name' }, band), el('ul', { class: 'rig-list' }, ...list.map((r) => rigRow(r, false)))),
            )),
      ),
    );
  }

  /** Refetch the rig list (and the tags) when the current rig is renamed, saved, created, swapped or retagged. */
  function onState(s: AppState) {
    const rig = s.rig;
    // The engine notes a rig as recent when it is loaded or saved (also a new rig's first save).
    const savedId = rig && s.saved ? rig.id : '';
    if (savedId && savedId !== lastSavedRig) recents = [savedId, ...recents.filter((id) => id !== savedId)].slice(0, MAX_RECENTS);
    lastSavedRig = savedId;
    const key = libraryKey(s);
    if (key === lastKey) return;
    lastKey = key;
    if (tab === 'rigs') void refreshRigs();
  }

  renderTabs();
  content.replaceChildren(rigsTop, rigsList); // the search field works before the first listRigs reply
  tagBar.hidden = true;
  root.toggleAttribute('inert', !root.classList.contains('open'));
  root.setAttribute('aria-hidden', String(!root.classList.contains('open')));
  lastKey = libraryKey(store.get());
  store.subscribe(onState);
  void refreshRigs();
}
