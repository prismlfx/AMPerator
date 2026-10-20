// Library panel (slides out from the left in both views): bands → rigs, favorites, recents, New rig, and read-only
// Models / IRs / Plugins lists. "Search online" is a placeholder tab (sub-project 3).

import { call } from '../bridge';
import { confirmDialog, el, toast } from '../dialog';
import { confirmDiscardIfDirty, setView, store, type AppState } from '../state';

interface RigEntry {
  id: string;
  name: string;
  band: string;
  favorite: boolean;
}

type Tab = 'rigs' | 'models' | 'irs' | 'plugins';

const MAX_RECENTS = 10; // Settings::maxRecent

let panel: HTMLElement | null = null;
let onToggle: ((open: boolean) => void) | null = null;

/** Opens or closes the panel rendered by renderLibrary. Returns the new state. */
export function toggleLibrary(open = !panel?.classList.contains('open')): boolean {
  if (!panel) return false;
  panel.classList.toggle('open', open);
  panel.toggleAttribute('inert', !open);
  panel.setAttribute('aria-hidden', String(!open));
  onToggle?.(open);
  return open;
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

export function renderLibrary(root: HTMLElement): void {
  panel = root;
  root.classList.add('library');
  root.setAttribute('aria-label', 'Library');

  let tab: Tab = 'rigs';
  let rigs: RigEntry[] = [];
  let recents = [...store.get().recents];
  let lastKey = '';
  let lastSavedRig = store.get().saved ? (store.get().rig?.id ?? '') : ''; // id of the current rig once saved

  const close = el('button', { class: 'icon-button', 'aria-label': 'Close library', title: 'Close' }, '×');
  close.addEventListener('click', () => toggleLibrary(false));
  const tabs = el('div', { class: 'library-tabs', role: 'tablist' });
  const content = el('div', { class: 'library-content', role: 'tabpanel' });
  root.replaceChildren(el('header', { class: 'library-header' }, el('h2', {}, 'Library'), close), tabs, content);

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
    if (tab === 'rigs') return refreshRigs();
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

  async function refreshRigs() {
    try {
      const r = await call<{ rigs: RigEntry[]; bands: string[] }>('listRigs');
      rigs = r.rigs;
    } catch (e) {
      toast(`Couldn't list rigs: ${(e as Error).message}`);
    }
    if (tab === 'rigs') renderRigs();
  }

  function rigRow(r: RigEntry, showBand: boolean) {
    const current = store.get().rig?.id === r.id;
    const load = el('button', { class: 'rig-load', title: `Open ${r.name}` }, r.name);
    if (current) load.setAttribute('aria-current', 'true');
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
    return el('li', { class: `rig-row${current ? ' current' : ''}` }, load, showBand ? el('span', { class: 'muted rig-band' }, r.band) : '', star, del);
  }

  function renderRigs() {
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

    const byId = new Map(rigs.map((r) => [r.id, r]));
    const favorites = rigs.filter((r) => r.favorite);
    const recent = recents.map((id) => byId.get(id)).filter((r): r is RigEntry => !!r);
    const bands: [string, RigEntry[]][] = [];
    for (const r of rigs) {
      const last = bands.at(-1);
      if (last && last[0] === r.band) last[1].push(r);
      else bands.push([r.band, [r]]);
    }

    const section = (title: string, cls: string, rows: HTMLElement[]) =>
      el('section', { class: `library-section ${cls}` }, el('h3', {}, title), el('ul', { class: 'rig-list' }, ...rows));

    content.replaceChildren(
      newRig,
      ...(favorites.length ? [section('Favorites', 'rig-shortcuts', favorites.map((r) => rigRow(r, true)))] : []),
      ...(recent.length ? [section('Recent', 'rig-shortcuts', recent.map((r) => rigRow(r, true)))] : []),
      el(
        'section',
        { class: 'library-section' },
        el('h3', {}, 'Bands'),
        ...(bands.length === 0
          ? [el('p', { class: 'muted' }, 'No saved rigs yet. Build one on the pedalboard and press Save.')]
          : bands.map(([band, list]) =>
              el('div', { class: 'band-group' }, el('h4', { class: 'band-name' }, band), el('ul', { class: 'rig-list' }, ...list.map((r) => rigRow(r, false)))),
            )),
      ),
    );
  }

  /** Refetch the rig list when the current rig is renamed, saved, created or swapped. */
  function onState(s: AppState) {
    const rig = s.rig;
    // The engine notes a rig as recent when it is loaded or saved (also a new rig's first save).
    const savedId = rig && s.saved ? rig.id : '';
    if (savedId && savedId !== lastSavedRig) recents = [savedId, ...recents.filter((id) => id !== savedId)].slice(0, MAX_RECENTS);
    lastSavedRig = savedId;
    const key = JSON.stringify([rig?.id, rig?.name, rig?.band, s.saved]);
    if (key === lastKey) return;
    lastKey = key;
    if (tab === 'rigs') void refreshRigs();
  }

  renderTabs();
  root.toggleAttribute('inert', !root.classList.contains('open'));
  root.setAttribute('aria-hidden', String(!root.classList.contains('open')));
  lastKey = JSON.stringify([store.get().rig?.id, store.get().rig?.name, store.get().rig?.band, store.get().saved]);
  store.subscribe(onState);
  void refreshRigs();
}
