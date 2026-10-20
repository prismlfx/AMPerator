// App shell: top bar (library, band / rig name, dirty dot, Save, device status, Stage/Edit, gear), banners, the
// library panel, the current view, toasts.

import './styles.css';
import { call } from './bridge';
import { el, toast } from './dialog';
import {
  confirmDiscardIfDirty,
  dismissStartupError,
  initStore,
  onToastableError,
  reportSummary,
  rigSaved,
  setRenamePending,
  setView,
  store,
  type AppState,
} from './state';
import { mountLearnOverlay, renderEdit } from './views/edit';
import { renderStage } from './views/stage';
import { onLibraryToggle, renderLibrary, toggleLibrary } from './views/library';
import { openSettings } from './views/settings';
import { retryBlacklistedPlugin, saveRigAs, watchQuitRequests } from './rigActions';

const app = document.getElementById('app')!;

// ---- top bar ----
const libraryButton = el('button', { class: 'icon-button', 'aria-label': 'Library', 'aria-expanded': 'false', title: 'Library' }, '☰');
libraryButton.addEventListener('click', () => toggleLibrary());
onLibraryToggle((open) => libraryButton.setAttribute('aria-expanded', String(open)));

const bandInput = el('input', { class: 'rig-band-input', type: 'text', 'aria-label': 'Band', placeholder: 'Band', list: 'band-names', spellcheck: 'false' });
const nameInput = el('input', { class: 'rig-name-input', type: 'text', 'aria-label': 'Rig name', placeholder: 'Rig name', spellcheck: 'false' });
const bandList = el('datalist', { id: 'band-names' });
const dirtyDot = el('span', { class: 'dirty', role: 'status' });
const saveButton = el('button', { class: 'button primary small', title: 'Save (⌘S)' }, 'Save');
const saveAsButton = el('button', { class: 'button small', title: 'Save a copy as a new rig (⇧⌘S)' }, 'Save as…');
saveAsButton.addEventListener('click', () => void saveRigAs());
const deviceChip = el('div', { class: 'device-chip' });
const stageButton = el('button', { class: 'segment', 'aria-pressed': 'false' }, 'Stage');
const editButton = el('button', { class: 'segment', 'aria-pressed': 'false' }, 'Edit');
stageButton.addEventListener('click', () => setView('stage'));
editButton.addEventListener('click', () => setView('edit'));
const gear = el('button', { class: 'icon-button gear', 'aria-label': 'Settings', title: 'Settings' });
gear.innerHTML =
  '<svg viewBox="0 0 24 24" width="22" height="22" aria-hidden="true"><path fill="currentColor" d="M19.4 13a7.5 7.5 0 0 0 0-2l2.1-1.6-2-3.5-2.5 1a7.3 7.3 0 0 0-1.7-1L15 3.3h-4l-.4 2.6a7.3 7.3 0 0 0-1.7 1l-2.5-1-2 3.5L6.6 11a7.5 7.5 0 0 0 0 2l-2.1 1.6 2 3.5 2.5-1a7.3 7.3 0 0 0 1.7 1l.4 2.6h4l.4-2.6a7.3 7.3 0 0 0 1.7-1l2.5 1 2-3.5zM13 15.5a3.5 3.5 0 1 1 0-7 3.5 3.5 0 0 1 0 7z" transform="translate(-1 0)"/></svg>';
gear.addEventListener('click', openSettings);

const banners = el('div', { class: 'banners' });
const libraryPanel = el('aside', { class: 'library' });
const editRoot = el('main', { class: 'view view-edit' });
const stageRoot = el('main', { class: 'view view-stage' });

app.append(
  el(
    'header',
    { class: 'app-header' },
    el('div', { class: 'brand' }, 'AMPerator'),
    libraryButton,
    el('div', { class: 'rig-fields' }, bandInput, el('span', { class: 'rig-sep', 'aria-hidden': 'true' }, '›'), nameInput, dirtyDot, bandList),
    saveButton,
    saveAsButton,
    el('div', { class: 'spacer' }),
    deviceChip,
    el('div', { class: 'segmented', role: 'group', 'aria-label': 'View' }, stageButton, editButton),
    gear,
  ),
  banners,
  el('div', { class: 'workspace' }, libraryPanel, editRoot, stageRoot),
);

// ---- rig name / band: edited locally, written by Save (saveRig {name, band}) ----
let draftRigId = '';
let draftEdited = false;

function syncRigFields(s: AppState) {
  const rig = s.rig;
  for (const i of [bandInput, nameInput]) i.disabled = !rig;
  saveButton.disabled = !rig;
  saveAsButton.disabled = !rig;
  if (!rig) {
    bandInput.value = nameInput.value = '';
    dirtyDot.textContent = '';
    setRenamePending(false);
    return;
  }
  if (rig.id !== draftRigId) {
    draftRigId = rig.id;
    draftEdited = false;
  }
  if (!draftEdited) {
    bandInput.value = rig.band;
    nameInput.value = rig.name;
  }
  const renamed = draftEdited && (nameInput.value.trim() !== rig.name || bandInput.value.trim() !== rig.band);
  setRenamePending(renamed); // confirmDiscardIfDirty counts it as unsaved changes
  const unsaved = s.dirty || renamed || !s.saved;
  dirtyDot.textContent = unsaved ? '●' : '';
  dirtyDot.title = unsaved ? (s.saved ? 'Unsaved changes' : 'Not saved yet') : '';
  dirtyDot.setAttribute('aria-label', dirtyDot.title);
  bandList.replaceChildren(...s.bands.map((b) => el('option', { value: b })));
}

for (const input of [bandInput, nameInput]) {
  input.addEventListener('input', () => {
    draftEdited = true;
    syncRigFields(store.get());
  });
  input.addEventListener('keydown', (e) => {
    if (e.key === 'Enter') void save();
    else if (e.key === 'Escape') {
      draftEdited = false;
      syncRigFields(store.get());
      input.blur();
    }
  });
}

async function save() {
  if (!store.get().rig) return;
  saveButton.disabled = true;
  try {
    const r = await call<{ id: string; name: string; band: string }>('saveRig', { name: nameInput.value, band: bandInput.value });
    draftEdited = false;
    rigSaved(r.name, r.band);
    toast(`Saved “${r.name}”`, 'info');
  } catch (e) {
    toast(`Couldn't save: ${(e as Error).message}`);
  } finally {
    syncRigFields(store.get());
  }
}
saveButton.addEventListener('click', () => void save());
document.addEventListener('keydown', (e) => {
  if ((e.metaKey || e.ctrlKey) && e.key.toLowerCase() === 's') {
    e.preventDefault();
    void (e.shiftKey ? saveRigAs() : save());
  }
});

let connectionError = '';
let dismissedReport = ''; // the load report whose banner the user closed

function render(s: AppState) {
  syncRigFields(s);

  const d = s.deviceStatus;
  deviceChip.className = `device-chip status-${d.status}`;
  deviceChip.textContent = d.status === 'Running' ? `${d.latencyMs.toFixed(1)} ms` : d.status === 'Disconnected' ? 'Audio disconnected' : 'No audio';
  deviceChip.title = d.status === 'Running' ? 'Round-trip latency' : '';

  stageButton.setAttribute('aria-pressed', String(s.view === 'stage'));
  editButton.setAttribute('aria-pressed', String(s.view === 'edit'));
  editRoot.hidden = s.view !== 'edit';
  stageRoot.hidden = s.view !== 'stage';

  renderBanners(s);
}

function renderBanners(s: AppState) {
  const items: Node[] = [];
  if (connectionError) items.push(banner(`Couldn't reach the engine: ${connectionError}`));
  if (s.deviceStatus.status === 'Disconnected')
    items.push(banner('The audio device was disconnected. Output is silent until it comes back.'));
  const summary = reportSummary(s.report);
  const reportKey = JSON.stringify([s.rig?.id, s.report]);
  if (summary && reportKey !== dismissedReport)
    items.push(
      banner(summary, () => {
        dismissedReport = reportKey;
        render(store.get());
      }),
    );
  s.startupErrors.forEach((e, i) => {
    const dismiss = () => dismissStartupError(i);
    if (e.context === 'loadRig' && e.rigId) {
      const restore = el('button', { class: 'button small' }, 'Open last backup');
      const rigId = e.rigId;
      restore.addEventListener('click', async () => {
        if (!(await confirmDiscardIfDirty())) return; // restoring loads that rig over the current one
        try {
          await call('restoreBackup', { id: rigId });
          dismiss();
        } catch (err) {
          toast((err as Error).message);
        }
      });
      items.push(banner(`Couldn't open '${rigId}': ${e.message}`, dismiss, restore));
    } else if (e.context === 'pluginBlacklisted' && e.pluginId) {
      const retry = el('button', { class: 'button small' }, 'Retry');
      const pluginId = e.pluginId;
      retry.addEventListener('click', async () => {
        if (await retryBlacklistedPlugin(pluginId)) dismiss();
      });
      items.push(banner(e.message, dismiss, retry));
    } else {
      items.push(banner(e.message, dismiss));
    }
  });
  banners.replaceChildren(...items);
}

function banner(text: string, onDismiss?: () => void, action?: HTMLElement) {
  const b = el('div', { class: 'banner', role: 'alert' }, el('span', { class: 'banner-text' }, text));
  if (action) b.append(action);
  if (onDismiss) {
    const x = el('button', { class: 'icon-button', 'aria-label': 'Dismiss' }, '×');
    x.addEventListener('click', onDismiss);
    b.append(x);
  }
  return b;
}

renderStage(stageRoot);
renderEdit(editRoot);
mountLearnOverlay(app);
renderLibrary(libraryPanel);
store.subscribe(render);
render(store.get());

onToastableError((e) => toast(e.message ?? 'Unknown error'));
watchQuitRequests();

initStore().catch((e) => {
  connectionError = (e as Error).message;
  render(store.get());
});
