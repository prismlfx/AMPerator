// TONE3000 integration, UI side: wire shapes (spec §7.4), license labels, connecting, and the download-job registry
// fed by the engine's `tone3000Download` events. Everything goes through the bridge; nothing here talks to the network.

import { call, on } from '../bridge';
import { toast } from '../dialog';
import logoSvg from '../brand/tone3000/logo.svg?raw';
import markSvg from '../brand/tone3000/mark.svg?raw';

export interface UserRef {
  username: string;
  displayName: string;
  avatarUrl: string;
  url: string;
}

export interface Tone3000Status {
  configured: boolean;
  connected: boolean;
  busy: boolean;
  remembered: boolean;
  user: UserRef | null;
}

export interface ToneSummary {
  id: string;
  title: string;
  gear: string;
  format: string; // "nam" | "ir"
  license: string;
  creator: UserRef;
  image: string;
  url: string;
  modelsCount: number;
  downloadsCount: number;
  favoritesCount: number;
}

export interface ToneModel {
  id: string;
  name: string;
  size: string;
  architecture: string;
}

export interface Tone extends ToneSummary {
  description: string;
  makes: string[];
  tags: string[];
  images: string[];
  publishedAt: string;
  models: ToneModel[];
}

export interface DownloadJob {
  jobId: string;
  modelId: string;
  toneId: string;
  nodeId?: string;
  state: 'queued' | 'downloading' | 'verifying' | 'done' | 'failed';
  progress: number;
  file?: string;
  sha256?: string;
  error?: string;
  updated?: boolean; // done: the file differs from the one the rig was saved with
}

export const NO_TONE3000: Tone3000Status = { configured: false, connected: false, busy: false, remembered: false, user: null };

export const TONE3000_URL = 'https://www.tone3000.com';
export const LONG_CALL_MS = 600_000; // sign-in and the picker wait for the user
export const LIST_CALL_MS = 30_000;

const LICENSES: Record<string, string> = {
  t3k: 'TONE3000 license',
  'cc-by': 'CC BY — credit the creator',
  'cc-by-sa': 'CC BY-SA — credit, share alike',
  'cc-by-nc': 'CC BY-NC — credit the creator, non-commercial',
  'cc-by-nc-sa': 'CC BY-NC-SA — credit, non-commercial, share alike',
  'cc-by-nc-nd': 'CC BY-NC-ND — credit, non-commercial, no derivatives',
  'cc-by-nd': 'CC BY-ND — credit, no derivatives',
  cco: 'CC0 — public domain',
};

export function licenseLabel(id: string): string {
  return LICENSES[id] ?? id;
}

// The stand-in drawings use currentColor; inside an <img> that is black, so paint them with the UI's text color.
const dataUrl = (svg: string) => `data:image/svg+xml;charset=utf-8,${encodeURIComponent(svg.replaceAll('currentColor', '#e8e9ec'))}`;
export const logoUrl = () => dataUrl(logoSvg);
export const markUrl = () => dataUrl(markSvg);

/** Remote pictures (avatars, tone images) are shown only from https URLs. */
export const remoteImage = (url: string | undefined): string => (url && url.startsWith('https://') ? url : '');

/** Opens the TONE3000 sign-in; resolves true when connected. Errors are toasted (a canceled sign-in is not one). */
export async function connectTone3000(): Promise<boolean> {
  try {
    const r = await call<{ connected?: boolean; canceled?: boolean }>('tone3000Connect', undefined, LONG_CALL_MS);
    return !r.canceled && !!r.connected;
  } catch (e) {
    toast((e as Error).message);
    return false;
  }
}

/** The model and tone ids of everything in the library that came from TONE3000 (listAssets sources). */
export async function libraryIndex(): Promise<{ models: Set<string>; tones: Set<string> }> {
  const models = new Set<string>();
  const tones = new Set<string>();
  const lists = await Promise.all(
    (['nam', 'ir'] as const).map((kind) => call<{ assets: { source?: { provider?: string; id?: string; toneId?: string } }[] }>('listAssets', { kind })),
  );
  for (const { assets } of lists)
    for (const a of assets) {
      const s = a.source;
      if (!s || (s.provider && s.provider !== 'tone3000')) continue;
      if (s.id) models.add(String(s.id));
      if (s.toneId) tones.add(String(s.toneId));
    }
  return { models, tones };
}

// ---- Download jobs ----

const jobs = new Map<string, DownloadJob>(); // insertion order = order of the latest event
const titles = new Map<string, string>(); // jobId -> title, remembered when the UI starts a download
const pendingTitles = new Map<string, string>(); // modelId -> title of a download whose reply is not in yet
const listeners = new Set<(job: DownloadJob) => void>();
let listening = false;
const MAX_FINISHED = 50;

const isTerminal = (j: DownloadJob) => j.state === 'done' || j.state === 'failed';

function titleOf(e: DownloadJob): string {
  let title = titles.get(e.jobId);
  if (title === undefined && pendingTitles.has(e.modelId)) {
    title = pendingTitles.get(e.modelId)!; // an event that beat the reply
    titles.set(e.jobId, title);
  }
  return title ?? 'the model';
}

function prune() {
  const finished = [...jobs.values()].filter(isTerminal);
  for (const j of finished.slice(0, Math.max(0, finished.length - MAX_FINISHED))) {
    jobs.delete(j.jobId);
    titles.delete(j.jobId);
  }
}

function onJobEvent(e: DownloadJob | undefined) {
  if (!e || typeof e.jobId !== 'string') return;
  const previous = jobs.get(e.jobId);
  jobs.delete(e.jobId);
  jobs.set(e.jobId, e);
  const title = titleOf(e);
  if (previous?.state !== e.state) {
    if (e.state === 'done') {
      toast(e.updated ? `${title} was updated on TONE3000 since this rig was saved` : `${title} added to your library`, 'info');
    } else if (e.state === 'failed' && e.error !== 'canceled') { // the user's own cancel (or a disconnect) needs no toast
      toast(e.error === 'gone' ? `${title} is no longer on TONE3000` : `Couldn't download ${title}: ${e.error ?? 'unknown error'}`);
    }
  }
  prune();
  for (const fn of [...listeners]) fn(e);
}

/** Starts listening for `tone3000Download` events (idempotent; called by initStore and by everything below). */
export function startJobEvents(): void {
  if (listening) return;
  listening = true;
  on('tone3000Download', onJobEvent);
}

/** `fn` runs for every download event. Returns the unsubscribe function. */
export function onJob(fn: (job: DownloadJob) => void): () => void {
  startJobEvents();
  listeners.add(fn);
  return () => listeners.delete(fn);
}

/** The latest event of every job of model `modelId`. */
export function jobsFor(modelId: string): DownloadJob[] {
  return [...jobs.values()].filter((j) => j.modelId === modelId);
}

export interface DownloadArgs {
  modelId: string;
  toneId?: string;
  nodeId?: string;
  addNode?: { type: 'NamAmp' | 'CabIR'; index: number };
}

/** tone3000Download; `title` names the download in the toasts its events produce (known before the reply, so an
    early event is named too). Resolves with the job id. */
export async function startDownload(args: DownloadArgs, title?: string): Promise<string> {
  startJobEvents();
  if (title) pendingTitles.set(args.modelId, title);
  try {
    const { jobId } = await call<{ jobId: string }>('tone3000Download', args);
    if (title && !titles.has(jobId)) titles.set(jobId, title);
    return jobId;
  } finally {
    if (title && pendingTitles.get(args.modelId) === title) pendingTitles.delete(args.modelId);
  }
}

// ---- Where a rig node's file came from ----

/** The provenance listAssets reports for a library file that came from TONE3000. */
export interface AssetSource {
  provider?: string;
  id?: string; // the model id (what a rig node's asset.source.id holds)
  toneId?: string;
  title?: string;
  creator?: { username?: string };
  license?: string;
  url?: string;
  image?: string;
}

let sources: Promise<Map<string, AssetSource>> | null = null;
let invalidating = false;

function watchSources() {
  if (invalidating) return;
  invalidating = true;
  on('rigLoaded', () => (sources = null));
  onJob((j) => {
    if (j.state === 'done') sources = null;
  });
}

/** Provenance of the library's TONE3000 files, by model id (listAssets nam + ir). Cached; dropped when a download
    finishes or a rig is loaded (`fresh` skips the cache, for a caller that runs before those listeners do). A failing
    lookup resolves to an empty map and is not cached. */
export function sourceInfo(fresh = false): Promise<Map<string, AssetSource>> {
  watchSources();
  if (sources && !fresh) return sources;
  const lookup = Promise.all(
    (['nam', 'ir'] as const).map((kind) => call<{ assets: { source?: AssetSource }[] }>('listAssets', { kind })),
  ).then(
    (lists) => {
      const map = new Map<string, AssetSource>();
      for (const { assets } of lists)
        for (const a of assets) if (a.source?.id && (!a.source.provider || a.source.provider === 'tone3000')) map.set(String(a.source.id), a.source);
      return map;
    },
    () => {
      if (sources === lookup) sources = null;
      return new Map<string, AssetSource>();
    },
  );
  sources = lookup;
  return lookup;
}
