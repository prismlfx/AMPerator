// App shell: header (rig, device status, gear), banners, the current view, toasts.

import './styles.css';
import { call } from './bridge';
import { el, toast } from './dialog';
import { confirmDiscardIfDirty, dismissStartupError, initStore, onToastableError, store, type AppState, type RigNode } from './state';
import { openSettings } from './views/settings';

const app = document.getElementById('app')!;

const rigTitle = el('div', { class: 'rig-title' });
const deviceChip = el('div', { class: 'device-chip' });
const gear = el('button', { class: 'icon-button gear', 'aria-label': 'Settings', title: 'Settings' });
gear.innerHTML =
  '<svg viewBox="0 0 24 24" width="22" height="22" aria-hidden="true"><path fill="currentColor" d="M19.4 13a7.5 7.5 0 0 0 0-2l2.1-1.6-2-3.5-2.5 1a7.3 7.3 0 0 0-1.7-1L15 3.3h-4l-.4 2.6a7.3 7.3 0 0 0-1.7 1l-2.5-1-2 3.5L6.6 11a7.5 7.5 0 0 0 0 2l-2.1 1.6 2 3.5 2.5-1a7.3 7.3 0 0 0 1.7 1l.4 2.6h4l.4-2.6a7.3 7.3 0 0 0 1.7-1l2.5 1 2-3.5zM13 15.5a3.5 3.5 0 1 1 0-7 3.5 3.5 0 0 1 0 7z" transform="translate(-1 0)"/></svg>';
gear.addEventListener('click', openSettings);

const banners = el('div', { class: 'banners' });
const view = el('main', { class: 'view' });

app.append(
  el('header', { class: 'app-header' }, el('div', { class: 'brand' }, 'AMPerator'), rigTitle, el('div', { class: 'spacer' }), deviceChip, gear),
  banners,
  view,
);

let connectionError = '';

function render(s: AppState) {
  rigTitle.replaceChildren(
    ...(s.rig
      ? [
          el('span', { class: 'band' }, s.rig.band),
          el('span', { class: 'rig-name' }, s.rig.name),
          ...(s.dirty ? [el('span', { class: 'dirty', title: 'Unsaved changes' }, '●')] : []),
        ]
      : []),
  );

  const d = s.deviceStatus;
  deviceChip.className = `device-chip status-${d.status}`;
  deviceChip.textContent = d.status === 'Running' ? `${d.latencyMs.toFixed(1)} ms` : d.status === 'Disconnected' ? 'Audio disconnected' : 'No audio';
  deviceChip.title = d.status === 'Running' ? 'Round-trip latency' : '';

  renderBanners(s);
  renderView(s);
}

function renderBanners(s: AppState) {
  const items: Node[] = [];
  if (connectionError) items.push(banner(`Couldn't reach the engine: ${connectionError}`));
  if (s.deviceStatus.status === 'Disconnected')
    items.push(banner('The audio device was disconnected. Output is silent until it comes back.'));
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

// Placeholder until the stage and edit views exist: the current rig's signal chain.
function renderView(s: AppState) {
  if (!s.connected) {
    view.replaceChildren(el('p', { class: 'muted center' }, connectionError ? '' : 'Connecting to the engine…'));
    return;
  }
  if (!s.rig) {
    view.replaceChildren(el('p', { class: 'muted center' }, 'No rig loaded.'));
    return;
  }
  view.replaceChildren(
    el('h1', { class: 'stage-rig-name' }, s.rig.name),
    el(
      'ol',
      { class: 'chain' },
      ...s.rig.nodes.map((n) =>
        el('li', { class: `chain-node${n.bypass ? ' bypassed' : ''}${s.report.placeholderNodeIds.includes(n.id) ? ' placeholder' : ''}` }, nodeLabel(n)),
      ),
    ),
  );
}

function nodeLabel(n: RigNode): string {
  const plugin = n.plugin as { name?: string } | undefined;
  const asset = n.asset as { file?: string } | undefined;
  if (plugin?.name) return plugin.name;
  if (asset?.file) return `${n.type}: ${asset.file.split('/').pop()!.replace(/\.[^.]+$/, '')}`;
  return n.type;
}

store.subscribe(render);
render(store.get());

onToastableError((e) => toast(e.message ?? 'Unknown error'));

initStore().catch((e) => {
  connectionError = (e as Error).message;
  render(store.get());
});
