// App state mirrored from the engine: seeded by getState, then kept current by bridge events
// (the engine announces every state change, so events are the single source of truth).

import { call, on } from './bridge';
import { confirmDialog } from './dialog';

export type View = 'stage' | 'edit';

export interface RigNode {
  id: string;
  type: string;
  bypass: boolean;
  params: Record<string, number>;
  [key: string]: unknown;
}

export interface Mapping {
  control: string;
  action: string;
  node: string;
}

export interface Rig {
  id: string;
  name: string;
  band: string;
  tags?: string[]; // free-form labels (style, decade, band, song): setTags
  nodes: RigNode[];
  mappings?: Mapping[];
  [key: string]: unknown;
}

export interface LoadReport {
  missingAssets: string[];
  missingPlugins: string[];
  placeholderNodeIds: string[];
  resolvedByFilename: string[];
}

export interface DeviceStatus {
  status: 'Running' | 'Disconnected' | 'NoDevice';
  latencyMs: number;
  xruns: number;
}

export interface ControlStatus {
  hid: 'Connected' | 'NotFound' | 'PermissionDenied' | 'Busy';
  exclusive: boolean;
  lastKey?: number | null; // HID keycode of the most recent pedal press
  assigning?: string | null; // footswitch control waiting for its key
  midiDevices: string[];
}

export interface StartupError {
  message: string;
  context: string;
  rigId?: string;
  pluginId?: string; // context "pluginBlacklisted"
}

export interface Settings {
  audioDeviceType: string;
  audioDeviceName: string;
  inputChannel: number;
  outputFirstChannel: number;
  sampleRate: number;
  bufferSize: number;
  watchedFolders: string[];
  globalMappings: { control: string; action: string; node: string }[];
  footswitchKeys: Record<string, string>;
  footswitchDevice?: { vid: number; pid: number } | null; // a keyboard picked as the footswitch
  // getSettings / setSettings only:
  availableDevices?: string[];
  availableSampleRates?: number[];
  availableBufferSizes?: number[];
  availableInputChannels?: string[];
  availableOutputChannels?: string[];
}

export interface AppState {
  connected: boolean; // getState has answered
  view: View;
  rig: Rig | null;
  report: LoadReport;
  dirty: boolean;
  saved: boolean;
  learning: boolean;
  learnNode: string; // the node an armed learn will map (set by the UI; "" when unknown)
  renamePending: boolean; // the top bar holds an unsaved rig name/band edit
  saveCount: number; // bumped by every saveRig of the current rig (the library refreshes what it read from disk)
  startupErrors: StartupError[];
  settings: Partial<Settings>;
  deviceStatus: DeviceStatus;
  controlStatus: ControlStatus;
  tunerOn: boolean;
  favorites: string[];
  recents: string[];
  bands: string[];
}

const initialState: AppState = {
  connected: false,
  view: 'stage', // the app opens into stage mode
  rig: null,
  report: { missingAssets: [], missingPlugins: [], placeholderNodeIds: [], resolvedByFilename: [] },
  dirty: false,
  saved: false,
  learning: false,
  learnNode: '',
  renamePending: false,
  saveCount: 0,
  startupErrors: [],
  settings: {},
  deviceStatus: { status: 'NoDevice', latencyMs: 0, xruns: -1 },
  controlStatus: { hid: 'NotFound', exclusive: false, midiDevices: [] },
  tunerOn: false,
  favorites: [],
  recents: [],
  bands: [],
};

let current: AppState = initialState;
const subscribers = new Set<(s: AppState) => void>();

function update(patch: Partial<AppState>) {
  current = { ...current, ...patch };
  for (const fn of [...subscribers]) fn(current);
}

function updateNode(id: string, change: (n: RigNode) => RigNode) {
  if (!current.rig) return;
  update({ rig: { ...current.rig, nodes: current.rig.nodes.map((n) => (n.id === id ? change(n) : n)) } });
}

export const store = {
  get(): AppState {
    return current;
  },
  subscribe(fn: (s: AppState) => void): () => void {
    subscribers.add(fn);
    return () => subscribers.delete(fn);
  },
};

export function setView(view: View) {
  update({ view });
}

/** After learnMapping succeeded (the engine arms learn; `learned` or a rig load ends it). */
export function setLearning(learning: boolean, node = '') {
  update({ learning, learnNode: learning ? node : '' });
}

/** The top bar's name/band inputs differ from the rig (unsaved): counts as unsaved changes. */
export function setRenamePending(renamePending: boolean) {
  if (current.renamePending !== renamePending) update({ renamePending });
}

/** After saveRig succeeded: the engine confirms the saved name/band in its reply (no rigLoaded). */
export function rigSaved(name: string, band: string) {
  if (current.rig) update({ rig: { ...current.rig, name, band }, saved: true, saveCount: current.saveCount + 1 });
}

/** setNodeArt's reply (the engine announces no event for it). */
export function patchNode(id: string, patch: Partial<RigNode>) {
  updateNode(id, (n) => ({ ...n, ...patch }));
}

/** After setTuner succeeded (the engine announces no event for it). */
export function setTunerOn(tunerOn: boolean) {
  if (current.tunerOn !== tunerOn) update({ tunerOn });
}

export function setSettings(settings: Partial<Settings>) {
  update({ settings });
}

export function dismissStartupError(index: number) {
  update({ startupErrors: current.startupErrors.filter((_, i) => i !== index) });
}

/** One line for the banner shown after a rig load that could not find everything (spec §5/§8), or null. */
export function reportSummary(r: LoadReport): string | null {
  const missing = [...r.missingAssets, ...r.missingPlugins].map((m) => m.split(/[\\/]/).pop() ?? m);
  const byName = r.resolvedByFilename.map((f) => f.split(/[\\/]/).pop() ?? f);
  const parts: string[] = [];
  if (missing.length > 0) parts.push(`Missing: ${missing.join(', ')} — those parts are bypassed and keep their settings.`);
  if (byName.length > 0) parts.push(`${byName.join(', ')} matched by file name (the file changed since the rig was saved).`);
  return parts.length > 0 ? parts.join(' ') : null;
}

/** Calls `fn` for engine `error` events that should be shown as a toast: not those delivered before getState
    answered (startup errors, shown as banners from getState), not repeats of a startup banner, and not a late
    `audioPermission` error (microphone access is answered after start-up), which becomes a banner itself. */
export function onToastableError(fn: (e: { message: string; context: string }) => void): () => void {
  return on('error', (e) => {
    if (!current.connected || e == null) return;
    const same = (s: StartupError) => s.message === e.message && s.context === e.context;
    if (current.startupErrors.some(same)) return;
    if (e.context === 'audioPermission') {
      update({ startupErrors: [...current.startupErrors, { message: e.message, context: e.context }] });
      return;
    }
    fn(e);
  });
}

let started = false;

/** Subscribes to engine events and loads the initial state. Call once at start-up. */
export async function initStore(): Promise<void> {
  if (!started) {
    started = true;
    on('rigLoaded', (d) => update({ rig: d.rig, report: d.report, saved: d.saved, learning: false }));
    on('rigDirty', (d) => update({ dirty: d.dirty }));
    on('tagsChanged', (d) => {
      if (current.rig) update({ rig: { ...current.rig, tags: [...(d?.tags ?? [])] } });
    });
    on('bypassChanged', (d) => updateNode(d.id, (n) => ({ ...n, bypass: d.bypass })));
    on('paramChanged', (d) => updateNode(d.id, (n) => ({ ...n, params: { ...n.params, [d.name]: d.value } })));
    on('deviceStatus', (d) => update({ deviceStatus: d }));
    on('learnCancelled', () => update({ learning: false, learnNode: '' }));
    on('mappingCleared', (d) => {
      const rig = current.rig;
      if (rig) update({ rig: { ...rig, mappings: (rig.mappings ?? []).filter((m) => m.control !== d.control) } });
    });
    on('controlStatus', (d) => update({ controlStatus: d }));
    on('learned', (d) => {
      // The control now toggles d.node in this rig, replacing whatever it mapped before.
      const rig = current.rig;
      if (!rig) return update({ learning: false });
      const mappings = (rig.mappings ?? []).filter((m) => m.control !== d.control);
      mappings.push({ control: d.control, action: 'toggleBypass', node: d.node });
      update({ learning: false, rig: { ...rig, mappings } });
    });
  }
  const s = await call<any>('getState');
  update({
    connected: true,
    rig: s.rig ?? null,
    report: s.report ?? initialState.report,
    dirty: !!s.dirty,
    saved: !!s.saved,
    learning: !!s.learning,
    startupErrors: s.startupErrors ?? [],
    settings: s.settings ?? {},
    deviceStatus: s.deviceStatus ?? initialState.deviceStatus,
    controlStatus: s.controlStatus ?? initialState.controlStatus,
    tunerOn: !!s.tunerOn,
    favorites: s.favorites ?? [],
    recents: s.recents ?? [],
    bands: s.bands ?? [],
  });
}

/** Before loadRig / newRig / nextRig / prevRig: resolves true when it is fine to replace the current rig
    (clean, or the user agreed to discard the unsaved changes). */
export async function confirmDiscardIfDirty(
  ask: (message: string) => Promise<boolean> = (m) => confirmDialog(m, 'Discard changes'),
): Promise<boolean> {
  if (!current.dirty && !current.renamePending) return true;
  const name = current.rig?.name ?? 'this rig';
  return ask(`“${name}” has unsaved changes. Discard them?`);
}
