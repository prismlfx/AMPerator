// Library tab "TONE3000" (spec §9.1–§9.3): a splash while signed out; when signed in a header, "Browse TONE3000" (the
// picker window) and the user's bounded lists (Favorites, Downloaded, Created, Trending, Latest). Nothing is fetched
// until the user acts, and list results are only held in memory.

import { call, openExternal } from '../bridge';
import { el, openModal, toast } from '../dialog';
import { archetypeUrl, guessArchetype } from '../art/archetypes';
import { store, type AppState, type RigNode } from '../state';
import { nodeTitle } from './edit';
import {
  connectTone3000,
  LIST_CALL_MS,
  libraryIndex,
  licenseLabel,
  logoUrl,
  LONG_CALL_MS,
  markUrl,
  onJob,
  remoteImage,
  startDownload,
  jobsFor,
  type DownloadArgs,
  type ToneModel,
  TONE3000_URL,
  type Tone,
  type Tone3000Status,
  type ToneSummary,
} from '../online/tone3000';

type ListId = 'favorited' | 'downloaded' | 'created' | 'trending' | 'latest';
type Format = 'nam' | 'ir';

const LISTS: [ListId, string][] = [
  ['favorited', 'Favorites'],
  ['downloaded', 'Downloaded'],
  ['created', 'Created'],
  ['trending', 'Trending'],
  ['latest', 'Latest'],
];

const EMPTY: Record<ListId, string> = {
  favorited: 'You have no favorites on TONE3000 yet.',
  downloaded: 'You have not downloaded anything from TONE3000 yet.',
  created: 'You have not published anything on TONE3000.',
  trending: 'Nothing is trending right now.',
  latest: 'Nothing new right now.',
};

/** Where a chosen model goes: an existing node (replace) or, by default, a new node. */
export interface DetailTarget {
  nodeId?: string;
}

const isIO = (n: RigNode) => n.type === 'Input' || n.type === 'Output';

/** The new node for a tone of `format` (spec §9.4), with `index` counting only the chain (non-Input/Output) nodes:
    NAM right after the last NamAmp, else before the first CabIR, else at the end; IR right after the last NamAmp or
    CabIR, else at the end. */
export function addToRigTarget(nodes: RigNode[], format: Format): { type: 'NamAmp' | 'CabIR'; index: number } {
  const chain = nodes.filter((n) => !isIO(n));
  const lastOf = (...types: string[]) => chain.reduce((at, n, i) => (types.includes(n.type) ? i : at), -1);
  if (format === 'nam') {
    const amp = lastOf('NamAmp');
    if (amp >= 0) return { type: 'NamAmp', index: amp + 1 };
    const cab = chain.findIndex((n) => n.type === 'CabIR');
    return { type: 'NamAmp', index: cab >= 0 ? cab : chain.length };
  }
  const last = lastOf('NamAmp', 'CabIR');
  return { type: 'CabIR', index: last >= 0 ? last + 1 : chain.length };
}

function modelLabel(m: ToneModel, inLibrary: boolean): string {
  const arch = m.architecture === '2' ? 'A2' : m.architecture === '1' ? 'A1' : '';
  return [m.name, m.size, arch].filter(Boolean).join(' · ') + (inLibrary ? ' (in library)' : '');
}

const ACTIVE = ['queued', 'downloading', 'verifying'];

/** The tone's detail window: model choice, Download / Add to rig (or Use on <node>), and the download's progress. */
export async function openToneDetail(toneId: string, target?: DetailTarget): Promise<void> {
  let tone: Tone;
  let known = new Set<string>();
  try {
    [tone, known] = await Promise.all([
      call<Tone>('tone3000Tone', { id: toneId }, LIST_CALL_MS),
      libraryIndex().then((i) => i.models, () => new Set<string>()),
    ]);
  } catch (e) {
    toast((e as Error).message);
    return;
  }

  const unsubscribes: (() => void)[] = [];
  const modal = openModal(tone.title, () => unsubscribes.forEach((u) => u()), 'tone-detail');
  const format = tone.format?.toLowerCase();
  const supported = format === 'nam' || format === 'ir';
  const models = tone.models ?? [];

  const image = remoteImage(tone.image);
  const heading = el('div', { class: 'tone-detail-head' });
  if (image) heading.append(el('img', { class: 'tone-detail-image', src: image, loading: 'lazy', referrerpolicy: 'no-referrer', alt: '' }));
  const facts = el('div', { class: 'tone-detail-facts' });
  const formatName = format === 'ir' ? 'IR' : format === 'nam' ? 'NAM' : (tone.format ?? '');
  facts.append(el('div', { class: 'tone-meta' }, ...(tone.gear ? [el('span', { class: 'tone-gear muted' }, tone.gear)] : []), el('span', { class: 'format-badge' }, formatName)));
  facts.append(el('div', { class: 'tone-creator' }, avatar(tone.creator?.avatarUrl), tone.creator?.username ?? ''));
  const licenseText = tone.license ? licenseLabel(tone.license) : '';
  if (licenseText) facts.append(el('div', { class: 'muted tone-license' }, licenseText));
  if ((tone.url ?? '').startsWith(`${TONE3000_URL}/`)) facts.append(link('View on TONE3000', tone.url));
  heading.append(facts);
  modal.body.append(heading);
  if (tone.description) modal.body.append(el('p', { class: 'tone-description' }, tone.description));

  if (models.length === 0) {
    modal.body.append(el('p', { class: 'muted' }, 'This tone has no downloadable models.'));
    return;
  }

  const select = el('select', { class: 'tone-models', 'aria-label': 'Model' });
  for (const m of models) select.append(el('option', { value: m.id }, modelLabel(m, known.has(m.id))));
  select.value = models[0].id;
  const selected = () => models.find((m) => m.id === select.value) ?? models[0];

  const rigNodes = () => store.get().rig?.nodes;
  const download = el('button', { class: 'button', type: 'button' }, 'Download');
  const place = target?.nodeId
    ? el('button', { class: 'button primary', type: 'button' }, `Use on ${nodeTitle(rigNodes()?.find((n) => n.id === target.nodeId) ?? ({ type: 'node' } as RigNode))}`)
    : el('button', { class: 'button', type: 'button' }, 'Add to rig');
  const actions = el('div', { class: 'modal-actions' }, target?.nodeId ? place : download, target?.nodeId ? download : place);
  const progress = el('div', { class: 'tone-job' });
  modal.body.append(el('label', { class: 'tone-model-label' }, 'Model', select), progress, actions);
  if (!supported) modal.body.append(el('p', { class: 'muted' }, "AMPerator can't load this format."));

  const start = async (extra: Omit<DownloadArgs, 'modelId' | 'toneId'>) => {
    const m = selected();
    try {
      await startDownload({ modelId: m.id, toneId: tone.id, ...extra }, `${tone.title} — ${m.name}`);
    } catch (e) {
      toast((e as Error).message);
    }
  };
  download.addEventListener('click', () => void start({}));
  place.addEventListener('click', () => {
    if (target?.nodeId) void start({ nodeId: target.nodeId });
    else if (rigNodes()) void start({ addNode: addToRigTarget(rigNodes()!, format as Format) });
  });

  function sync() {
    const job = jobsFor(selected().id).at(-1);
    const running = !!job && ACTIVE.includes(job.state);
    progress.replaceChildren();
    if (running) {
      const cancel = el('button', { class: 'button small', type: 'button' }, 'Cancel');
      cancel.addEventListener('click', () => void call('tone3000Cancel', { jobId: job.jobId }).catch((e) => toast((e as Error).message)));
      progress.append(el('progress', { max: '1', value: String(job.progress ?? 0), 'aria-label': 'Download progress' }), cancel);
    } else if (job?.state === 'failed') {
      progress.append(el('p', { class: 'tone-job-error' }, job.error === 'gone' ? 'This model is no longer on TONE3000.' : `Download failed: ${job.error ?? 'unknown error'}`));
    }
    download.disabled = !supported || running;
    place.disabled = !supported || running || (!target?.nodeId && !rigNodes());
  }
  select.addEventListener('change', sync);
  unsubscribes.push(onJob(sync), store.subscribe(sync));
  sync();
}

function link(label: string, url: string, cls = 'link-button'): HTMLButtonElement {
  const b = el('button', { class: cls, type: 'button' }, label);
  b.addEventListener('click', () => void openExternal(url));
  return b;
}

function avatar(url: string): HTMLElement | '' {
  const src = remoteImage(url);
  return src ? el('img', { class: 'avatar', src, loading: 'lazy', referrerpolicy: 'no-referrer', alt: '' }) : '';
}

/** Renders the tab into `content` and returns the function that tears its subscriptions down. */
export function renderTone3000Tab(content: HTMLElement): () => void {
  let disposed = false;
  let status: Tone3000Status = store.get().tone3000;
  let statusKey = '';
  let format: Format | null = null;
  let list: ListId = 'favorited';
  let page = 1;
  let result: { tones: ToneSummary[]; totalPages: number } | null = null;
  let error = '';
  let loading = false;
  let tonesInLibrary = new Set<string>();
  let request = 0;
  let browsing = false;

  let listArea: HTMLElement | null = null;
  let browseButtons: HTMLButtonElement[] = [];

  const keyOf = (s: Tone3000Status) => JSON.stringify([s.configured, s.connected, s.busy, s.user]);

  async function refreshIndex() {
    try {
      const index = await libraryIndex();
      if (disposed) return;
      tonesInLibrary = index.tones;
      renderList();
    } catch {
      // the marks just stay as they were
    }
  }

  async function browse() {
    if (browsing) return;
    browsing = true;
    syncBrowse();
    try {
      const r = await call<{ toneId?: string; nodeId?: string; canceled?: boolean }>(
        'tone3000Browse',
        format ? { format } : {},
        LONG_CALL_MS,
      );
      if (!r.canceled && r.toneId) void openToneDetail(r.toneId, r.nodeId ? { nodeId: r.nodeId } : undefined);
    } catch (e) {
      toast((e as Error).message);
    } finally {
      browsing = false;
      if (!disposed) syncBrowse();
    }
  }

  function syncBrowse() {
    for (const b of browseButtons) b.disabled = browsing || status.busy;
  }

  async function load() {
    const mine = ++request;
    loading = true;
    error = '';
    renderList();
    try {
      const r = await call<{ tones: ToneSummary[]; page: number; totalPages: number }>(
        'tone3000List',
        { list, page },
        LIST_CALL_MS,
      );
      if (disposed || mine !== request) return;
      result = { tones: r.tones ?? [], totalPages: r.totalPages ?? 1 };
      page = r.page ?? page;
    } catch (e) {
      if (disposed || mine !== request) return;
      result = null;
      error = (e as Error).message;
    }
    loading = false;
    renderList();
  }

  function toneRow(t: ToneSummary) {
    const fmt = t.format?.toLowerCase() === 'ir' ? 'ir' : 'nam';
    const src = remoteImage(t.image);
    const art = src
      ? el('img', { class: 'tone-image', src, loading: 'lazy', referrerpolicy: 'no-referrer', alt: '' })
      : el('img', { class: 'tone-image', src: archetypeUrl(guessArchetype(fmt === 'ir' ? 'CabIR' : 'NamAmp', t.title)), alt: '' });
    const open = el('button', { class: 'tone-open', type: 'button', title: `Open ${t.title}` }, el('span', { class: 'tone-title' }, t.title));
    open.addEventListener('click', () => void openToneDetail(t.id));
    const creator = el('span', { class: 'tone-creator muted' }, avatar(t.creator?.avatarUrl), t.creator?.username ?? '');
    const meta = el('div', { class: 'tone-meta' }, el('span', { class: 'format-badge' }, fmt === 'ir' ? 'IR' : 'NAM'), creator);
    if (t.gear) meta.prepend(el('span', { class: 'tone-gear muted' }, t.gear));
    const row = el('li', { class: 'tone-row' }, art, el('div', { class: 'tone-main' }, open, meta));
    if (tonesInLibrary.has(t.id)) row.append(el('img', { class: 'tone-mark', src: markUrl(), alt: 'T3K', title: 'In your library' }));
    return row;
  }

  function renderList() {
    if (!listArea) return;
    const parts: (Node | string)[] = [];
    if (loading && !result) parts.push(el('p', { class: 'muted' }, 'Loading…'));
    else if (error) {
      const retry = el('button', { class: 'button small', type: 'button' }, 'Retry');
      retry.addEventListener('click', () => void load());
      parts.push(el('div', { class: 'tone-error' }, el('p', { class: 'muted' }, error), retry));
    } else if (result) {
      if (result.tones.length === 0) parts.push(el('p', { class: 'muted' }, EMPTY[list]));
      else parts.push(el('ul', { class: 'tone-list', 'aria-busy': String(loading) }, ...result.tones.map(toneRow)));
      if (result.totalPages > 1) {
        const prev = el('button', { class: 'button small', type: 'button' }, 'Previous');
        const next = el('button', { class: 'button small', type: 'button' }, 'Next');
        prev.disabled = loading || page <= 1;
        next.disabled = loading || page >= result.totalPages;
        prev.addEventListener('click', () => {
          page = Math.max(1, page - 1);
          void load();
        });
        next.addEventListener('click', () => {
          page += 1;
          void load();
        });
        parts.push(el('div', { class: 'tone-pager' }, prev, el('span', { class: 'muted' }, `Page ${page} of ${result.totalPages}`), next));
      }
    }
    const discover = el('button', { class: 'button tone-discover', type: 'button' }, 'Discover more on TONE3000');
    discover.addEventListener('click', () => void browse());
    browseButtons = browseButtons.filter((b) => !b.classList.contains('tone-discover'));
    browseButtons.push(discover);
    syncBrowse();
    parts.push(discover);
    listArea.replaceChildren(...parts);
  }

  function renderSplash() {
    const connect = el('button', { class: 'button primary', type: 'button' }, 'Connect TONE3000');
    connect.disabled = status.busy;
    connect.addEventListener('click', async () => {
      connect.disabled = true;
      await connectTone3000(); // the engine's tone3000Status event switches the tab
      if (!disposed) connect.disabled = status.busy;
    });
    content.replaceChildren(
      el(
        'div',
        { class: 'tone-splash' },
        el('img', { class: 'tone-logo', src: logoUrl(), alt: 'TONE3000' }),
        el('p', {}, 'AMPerator has partnered with TONE3000 to give you access to thousands of NAM captures and IRs.'),
        connect,
        link('Powered by TONE3000', TONE3000_URL, 'link-button tone-powered'),
      ),
    );
  }

  function renderConnected() {
    const user = status.user;
    const disconnect = el('button', { class: 'button small', type: 'button' }, 'Disconnect');
    disconnect.addEventListener('click', async () => {
      disconnect.disabled = true;
      try {
        await call('tone3000Disconnect');
      } catch (e) {
        toast((e as Error).message);
        disconnect.disabled = false;
      }
    });
    const header = el(
      'div',
      { class: 'tone-account' },
      avatar(user?.avatarUrl ?? ''),
      el('span', { class: 'tone-user' }, user?.username ?? ''),
      disconnect,
    );

    const browseButton = el('button', { class: 'button primary', type: 'button' }, 'Browse TONE3000');
    browseButton.addEventListener('click', () => void browse());
    browseButtons = [browseButton];
    const chips = (['nam', 'ir'] as const).map((f) => {
      const chip = el('button', { class: 'tag-filter', type: 'button', 'aria-pressed': String(format === f) }, f === 'nam' ? 'NAM' : 'IR');
      chip.addEventListener('click', () => {
        format = format === f ? null : f;
        for (const [i, c] of chips.entries()) c.setAttribute('aria-pressed', String(format === (['nam', 'ir'] as const)[i]));
      });
      return chip;
    });
    const browseRow = el('div', { class: 'tone-browse' }, browseButton, el('div', { class: 'tone-formats', role: 'group', 'aria-label': 'Format' }, ...chips));

    const subtabs = el('div', { class: 'library-tabs tone-lists', role: 'tablist' });
    const renderSubtabs = () =>
      subtabs.replaceChildren(
        ...LISTS.map(([id, label]) => {
          const b = el('button', { class: 'library-tab', role: 'tab', type: 'button', 'aria-selected': String(id === list) }, label);
          b.addEventListener('click', () => {
            if (id === list) return;
            list = id;
            page = 1;
            result = null;
            renderSubtabs();
            void load();
          });
          return b;
        }),
      );
    renderSubtabs();

    listArea = el('div', { class: 'tone-results' });
    content.replaceChildren(header, browseRow, subtabs, listArea);
    renderList();
    void load();
    void refreshIndex();
  }

  function render() {
    listArea = null;
    browseButtons = [];
    if (!status.configured) content.replaceChildren(el('p', { class: 'muted' }, 'This build has no TONE3000 client id.'));
    else if (!status.connected) renderSplash();
    else renderConnected();
  }

  const unsubscribeStore = store.subscribe((s: AppState) => {
    const previous = status;
    status = s.tone3000;
    const key = keyOf(status);
    if (key === statusKey) return;
    statusKey = key;
    // A change of the busy flag alone only enables or disables buttons; signing in / out or another user redraws.
    if (listArea && previous.connected && status.connected && previous.user?.username === status.user?.username) {
      syncBrowse();
      return;
    }
    request++; // answers to the previous session's lists are dropped
    result = null;
    error = '';
    page = 1;
    render();
  });

  const unsubscribeJobs = onJob((job) => {
    if (job.state === 'done' && listArea) void refreshIndex();
  });

  statusKey = keyOf(status);
  render();

  return () => {
    disposed = true;
    request++;
    unsubscribeStore();
    unsubscribeJobs();
  };
}
