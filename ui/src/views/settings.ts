// Settings modal (gear icon): audio device and routing, latency/xruns, watched folders, footswitch and MIDI status.

import { call, invokeNative } from '../bridge';
import { el, openModal, toast } from '../dialog';
import { setSettings, store, type AppState, type ControlStatus, type DeviceStatus, type Settings } from '../state';

let openInstance: { close(): void } | null = null;

export function openSettings() {
  if (openInstance) return;

  let settings: Settings | null = null;
  let busy = false;
  let choosing = false; // the native folder chooser is open

  const form = el('div', { class: 'settings-form' }, el('p', { class: 'muted' }, 'Loading…'));
  const status = el('div', { class: 'settings-status' });
  const folders = el('div', { class: 'settings-folders' });
  const controls = el('div', { class: 'settings-controls' });

  const unsubscribe = store.subscribe((s) => renderLive(s));
  const modal = openModal('Settings', () => {
    if (store.get().controlStatus.assigning) void call('cancelAssignFootswitchKey').catch(() => {});
    unsubscribe();
    document.removeEventListener('keydown', onEsc, true);
    openInstance = null;
  }, 'modal-settings');
  openInstance = modal;

  modal.body.append(
    section('Audio', form, status),
    section('Watched folders', folders),
    section('Footswitch & MIDI', controls),
  );

  let lastDeviceStatus = store.get().deviceStatus.status;
  let wasAssigning = false;

  // Esc while waiting for a pedal press cancels the assign instead of closing the dialog.
  const onEsc = (e: KeyboardEvent) => {
    if (e.key === 'Escape' && store.get().controlStatus.assigning) {
      e.stopImmediatePropagation();
      void call('cancelAssignFootswitchKey').catch(() => {});
    }
  };
  document.addEventListener('keydown', onEsc, true);
  const footswitchActions: FootswitchActions = {
    assign: (control) => void call('assignFootswitchKey', { control }).catch((e) => toast((e as Error).message)),
    cancel: () => void call('cancelAssignFootswitchKey').catch(() => {}),
  };

  async function load() {
    try {
      accept(await call<Settings>('getSettings'));
    } catch (e) {
      toast(`Couldn't read settings: ${(e as Error).message}`);
    }
  }

  function accept(s: Settings) {
    settings = s;
    setSettings(s);
    renderForm();
    renderFolders();
  }

  async function apply(patch: Partial<Settings>) {
    busy = true;
    renderForm();
    renderFolders();
    try {
      accept(await call<Settings>('setSettings', patch));
    } catch (e) {
      toast((e as Error).message); // e.g. the device failed to open; other fields were still applied
      await load();
    } finally {
      busy = false;
      renderForm();
      renderFolders();
    }
  }

  /** The watched folders as of now (not as of when a button was rendered: a change may have landed since). */
  const currentFolders = () => settings?.watchedFolders ?? [];

  function renderForm() {
    if (!settings) return;
    const s = settings;
    const devices = [...(s.availableDevices ?? [])];
    if (s.audioDeviceName && !devices.includes(s.audioDeviceName)) devices.unshift(s.audioDeviceName);
    const deviceLabel = (d: string) => (s.availableDevices?.includes(d) ? d : `${d} (not connected)`);

    const inputs = s.availableInputChannels ?? [];
    const outputs = s.availableOutputChannels ?? [];
    const pairs: [number, string][] = [];
    for (let i = 0; i < outputs.length; i += 2)
      pairs.push([i, i + 1 < outputs.length ? `${outputs[i]} + ${outputs[i + 1]}` : outputs[i]]);
    if (outputs.length > 0 && !pairs.some(([i]) => i === s.outputFirstChannel) && s.outputFirstChannel + 1 < outputs.length)
      pairs.push([s.outputFirstChannel, `${outputs[s.outputFirstChannel]} + ${outputs[s.outputFirstChannel + 1]}`]);

    const rates = s.availableSampleRates ?? [];
    const buffers = s.availableBufferSizes ?? [];
    const sr = s.sampleRate > 0 ? s.sampleRate : 48000;

    form.replaceChildren(
      field('Device', select(devices.map((d) => [d, deviceLabel(d)]), s.audioDeviceName, (v) => apply({ audioDeviceName: v }), 'No audio devices')),
      field('Input channel', select(inputs.map((n, i) => [String(i), n]), String(s.inputChannel), (v) => apply({ inputChannel: Number(v) }), '—')),
      field('Output pair', select(pairs.map(([i, n]) => [String(i), n]), String(s.outputFirstChannel), (v) => apply({ outputFirstChannel: Number(v) }), '—')),
      field('Sample rate', select(rates.map((r) => [String(r), `${r} Hz`]), String(s.sampleRate), (v) => apply({ sampleRate: Number(v) }), '—')),
      field(
        'Buffer size',
        select(
          buffers.map((b) => [String(b), `${b} samples (${((b / sr) * 1000).toFixed(1)} ms)`]),
          String(s.bufferSize),
          (v) => apply({ bufferSize: Number(v) }),
          '—',
        ),
      ),
    );
    for (const sel of form.querySelectorAll('select')) sel.disabled = busy || sel.options.length === 0 || sel.dataset.empty === '1';
  }

  function renderFolders() {
    if (!settings) return;
    const list = settings.watchedFolders ?? [];
    const add = el('button', { class: 'button' }, choosing ? 'Choosing…' : 'Add folder…');
    add.disabled = busy || choosing;
    add.addEventListener('click', async () => {
      choosing = true;
      renderFolders();
      try {
        const path = await invokeNative('chooseFolder');
        const folders = currentFolders();
        if (typeof path === 'string' && path !== '' && !folders.includes(path)) {
          choosing = false;
          await apply({ watchedFolders: [...folders, path] });
        }
      } catch (e) {
        toast((e as Error).message);
      } finally {
        choosing = false;
        renderFolders();
      }
    });
    folders.replaceChildren(
      el('p', { class: 'muted' }, 'Models and IRs in these folders appear in the library, next to the ones in ~/Music/AMPerator.'),
      list.length === 0
        ? el('p', { class: 'muted' }, 'No watched folders.')
        : el(
            'ul',
            { class: 'folder-list' },
            ...list.map((f) => {
              const remove = el('button', { class: 'button small', title: `Stop watching ${f}` }, 'Remove');
              remove.disabled = busy;
              remove.addEventListener('click', () => {
                if (!busy) void apply({ watchedFolders: currentFolders().filter((x) => x !== f) });
              });
              return el('li', {}, el('span', { class: 'path', title: f }, f), remove);
            }),
          ),
      add,
    );
  }

  function renderLive(s: AppState) {
    status.replaceChildren(...deviceStatusView(s.deviceStatus));
    controls.replaceChildren(...controlStatusView(s.controlStatus, footswitchActions));
    const assigning = !!s.controlStatus.assigning;
    if (wasAssigning && !assigning && !busy) void load(); // the key map changed (or the assign was cancelled)
    wasAssigning = assigning;
    // Hot-plug: the device list and the open device's capabilities may have changed.
    if (s.deviceStatus.status !== lastDeviceStatus) {
      lastDeviceStatus = s.deviceStatus.status;
      if (!busy) void load();
    }
  }

  renderLive(store.get());
  void load();
}

function section(title: string, ...content: Node[]) {
  return el('section', { class: 'settings-section' }, el('h3', {}, title), ...content);
}

function field(label: string, control: HTMLElement) {
  return el('label', { class: 'field' }, el('span', { class: 'field-label' }, label), control);
}

function select(options: [string, string][], value: string, onChange: (v: string) => void, emptyLabel: string) {
  const s = el('select');
  const known = options.some(([v]) => v === value);
  if (options.length === 0) {
    s.append(el('option', { value: '' }, emptyLabel));
    s.dataset.empty = '1';
  } else if (!known) {
    // e.g. no device open yet: show an explicit prompt instead of silently pre-selecting the first entry
    const prompt = el('option', { value: '' }, 'Choose…');
    prompt.disabled = true;
    s.append(prompt);
  }
  for (const [v, label] of options) s.append(el('option', { value: v }, label));
  s.value = known ? value : '';
  s.addEventListener('change', () => {
    if (s.value !== '') onChange(s.value);
  });
  return s;
}

export function deviceStatusView(d: DeviceStatus): Node[] {
  const statusText = { Running: 'Running', Disconnected: 'Disconnected — reconnects automatically when the device returns', NoDevice: 'No audio device open' }[d.status];
  const out: Node[] = [
    el('div', { class: 'stat' }, el('span', { class: 'stat-label' }, 'Status'), el('span', { class: `stat-value status-${d.status}` }, statusText)),
    el(
      'div',
      { class: 'stat' },
      el('span', { class: 'stat-label' }, 'Round-trip latency'),
      el('span', { class: 'stat-value', 'data-testid': 'latency' }, d.status === 'Running' ? `${d.latencyMs.toFixed(1)} ms` : '—'),
    ),
    el('div', { class: 'stat' }, el('span', { class: 'stat-label' }, 'Xruns'), el('span', { class: 'stat-value' }, d.xruns < 0 ? '—' : String(d.xruns))),
  ];
  if (d.xruns > 0) out.push(el('p', { class: 'hint warn' }, 'Audio dropouts detected — try a larger buffer size.'));
  return out;
}

export interface FootswitchActions {
  assign(control: string): void;
  cancel(): void;
}

const PEDALS: [string, string][] = [
  ['footswitch:L', 'L'],
  ['footswitch:M', 'M'],
  ['footswitch:R', 'R'],
];

function keyAssignView(c: ControlStatus, actions: FootswitchActions): Node {
  const hex = (n: number) => '0x' + n.toString(16).toUpperCase().padStart(2, '0');
  const buttons = PEDALS.map(([control, label]) => {
    const waiting = c.assigning === control;
    const b = el('button', { type: 'button', class: waiting ? 'assigning' : '' }, waiting ? 'Press the pedal now…' : `Assign ${label}`);
    b.addEventListener('click', () => (waiting ? actions.cancel() : actions.assign(control)));
    return b;
  });
  return el(
    'div',
    { class: 'footswitch-keys' },
    el('p', {}, `Last key: ${c.lastKey == null ? 'none yet' : hex(c.lastKey)}`),
    el('div', { class: 'row' }, ...buttons),
    c.assigning ? el('p', { class: 'hint' }, 'Press the pedal you want for this slot (Esc to cancel).') : el('span'),
  );
}

export function controlStatusView(c: ControlStatus, actions?: FootswitchActions): Node[] {
  let hid: Node;
  switch (c.hid) {
    case 'Connected':
      hid = c.exclusive
        ? el('p', {}, el('strong', { class: 'ok' }, 'Connected'), ' — exclusive: presses only control AMPerator, even in the background.')
        : el(
            'p',
            {},
            el('strong', { class: 'ok' }, 'Connected'),
            ' — shared: presses also type into other apps — tip: set your pedal to send F13/F14/F15.',
          );
      break;
    case 'PermissionDenied':
      hid = el(
        'p',
        {},
        el('strong', { class: 'warn' }, 'Permission needed. '),
        'Open System Settings → Privacy & Security → Input Monitoring, enable AMPerator, then relaunch.',
      );
      break;
    case 'Busy':
      hid = el('p', {}, el('strong', { class: 'warn' }, 'In use by another app. '), 'Quit the other app that has the footswitch open.');
      break;
    default:
      hid = el('p', {}, el('strong', {}, 'No footswitch found. '), 'Plug in your USB footswitch; it is detected automatically.');
  }
  const midi =
    c.midiDevices.length === 0
      ? el('p', { class: 'muted' }, 'No MIDI input devices.')
      : el('ul', { class: 'midi-list' }, ...c.midiDevices.map((m) => el('li', {}, m)));
  const keys = actions && c.hid === 'Connected' ? [keyAssignView(c, actions)] : [];
  return [el('h4', {}, 'USB footswitch'), hid, ...keys, el('h4', {}, 'MIDI inputs'), midi];
}
