import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { installFakeJuce, type FakeJuce } from '../test-support';

let juce: FakeJuce;
let pickers: typeof import('./pickers');

const flush = () => new Promise((r) => setTimeout(r, 0));
const lastCommand = () => juce.commands().at(-1);

function plugin(name: string, vendor: string, format: string, more: object = {}) {
  return { format, uid: `${format}-${name}`, name, vendor, category: 'Effect', isInstrument: false, numInputChannels: 2, numOutputChannels: 2, ...more };
}
const PLUGINS = [
  plugin('kHs Delay', 'Kilohearts', 'VST3', { category: 'Fx|Delay' }),
  plugin('Guitar Rig 7', 'Native Instruments', 'VST3', { category: 'Fx|Distortion' }),
  plugin('Dirt', 'Native Instruments', 'AudioUnit'),
  plugin('SPAN', 'Voxengo', 'AudioUnit'),
  plugin('kHs Gain', 'Kilohearts', 'AudioUnit'),
  plugin('Kontakt 8', 'Native Instruments', 'AudioUnit', { category: 'Synth', isInstrument: true, numInputChannels: 0 }),
  plugin('Raum', 'Native Instruments', 'VST3'),
];

const modal = () => document.querySelector<HTMLElement>('.modal')!;
const search = () => modal().querySelector<HTMLInputElement>('input[type="search"]')!;
const guitarToggle = () => modal().querySelector<HTMLButtonElement>('.plugin-guitar-toggle')!;
const groups = () => [...modal().querySelectorAll<HTMLElement>('.plugin-group')];
const group = (id: string) => modal().querySelector<HTMLElement>(`.plugin-group[data-group="${id}"]`);
const header = (id: string) => group(id)!.querySelector<HTMLButtonElement>('.plugin-group-header')!;
const visibleRows = (id: string) => {
  const list = group(id)!.querySelector<HTMLElement>('.plugin-list')!;
  return list.hidden ? [] : [...list.querySelectorAll<HTMLButtonElement>('.plugin-row')];
};
const rowNames = (id: string) => visibleRows(id).map((b) => b.querySelector('.plugin-name')!.textContent);
const groupSummary = () =>
  groups().map((g) => [
    g.dataset.group,
    g.querySelector('.plugin-group-label')!.textContent,
    g.querySelector('.plugin-group-count')!.textContent,
  ]);
const typeSearch = (text: string) => {
  search().value = text;
  search().dispatchEvent(new Event('input', { bubbles: true }));
};
const key = (target: EventTarget, k: string) => target.dispatchEvent(new KeyboardEvent('keydown', { key: k, bubbles: true }));

async function open(plugins: object[] = PLUGINS) {
  pickers.openPluginPicker(3);
  const list = lastCommand();
  expect(list).toMatchObject({ cmd: 'listPlugins' });
  juce.reply(list.id, true, { plugins });
  await flush();
}

beforeEach(async () => {
  vi.resetModules();
  document.body.innerHTML = '';
  localStorage.clear();
  juce = installFakeJuce();
  pickers = await import('./pickers');
});

afterEach(() => {
  document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape' }));
  delete (window as any).__JUCE__;
});

describe('plugin picker', () => {
  it('opens with the search field focused, Guitar on and the guitar groups listed in order with counts', async () => {
    await open();
    expect(search().placeholder).toBe('Search plugins…');
    expect(document.activeElement).toBe(search());
    expect(guitarToggle().getAttribute('aria-pressed')).toBe('true');
    expect([...modal().querySelectorAll('button')].some((b) => b.textContent === 'Rescan')).toBe(true);
    expect(groupSummary()).toEqual([
      ['drive', 'Drive', '2'],
      ['amps-cabs', 'Amps & Cabs', '1'],
      ['delay', 'Delay', '1'],
      ['reverb', 'Reverb', '1'],
    ]);
    expect(rowNames('drive')).toEqual(['Dirt', 'Guitar Rig 7']); // in two groups: listed in both
    expect(rowNames('amps-cabs')).toEqual(['Guitar Rig 7']);
  });

  it('shows each row with its name, vendor and format badge', async () => {
    await open();
    const [dirt, rig] = visibleRows('drive');
    expect(dirt.querySelector('.plugin-vendor')!.textContent).toBe('Native Instruments');
    expect(dirt.querySelector('.plugin-format')!.textContent).toBe('AU');
    expect(rig.querySelector('.plugin-format')!.textContent).toBe('VST3');
  });

  it('Guitar off shows Utility & Other and Instruments, collapsed until their chevron is clicked; the choice is remembered', async () => {
    await open();
    guitarToggle().click();
    expect(guitarToggle().getAttribute('aria-pressed')).toBe('false');
    expect(groupSummary().map(([id, , n]) => [id, n])).toEqual([
      ['drive', '2'],
      ['amps-cabs', '1'],
      ['delay', '1'],
      ['reverb', '1'],
      ['utility', '2'],
      ['instrument', '1'],
    ]);
    expect(header('drive').getAttribute('aria-expanded')).toBe('true');
    expect(header('utility').getAttribute('aria-expanded')).toBe('false');
    expect(header('instrument').getAttribute('aria-expanded')).toBe('false');
    expect(rowNames('utility')).toEqual([]);

    header('utility').click();
    expect(header('utility').getAttribute('aria-expanded')).toBe('true');
    expect(rowNames('utility')).toEqual(['kHs Gain', 'SPAN']);
    header('drive').click(); // any group collapses
    expect(rowNames('drive')).toEqual([]);

    // Reopened: Guitar stays off.
    document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape' }));
    await open();
    expect(guitarToggle().getAttribute('aria-pressed')).toBe('false');
    expect(group('instrument')).not.toBeNull();
    guitarToggle().click();
    document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape' }));
    await open();
    expect(guitarToggle().getAttribute('aria-pressed')).toBe('true');
    expect(group('instrument')).toBeNull();
  });

  it('searches names and vendors, keeping the focus in the search field', async () => {
    await open();
    typeSearch('native');
    expect(groupSummary().map(([id, , n]) => [id, n])).toEqual([
      ['drive', '2'],
      ['amps-cabs', '1'],
      ['reverb', '1'],
    ]);
    expect(document.activeElement).toBe(search());
    typeSearch('kilohearts delay');
    expect(groupSummary().map(([id]) => id)).toEqual(['delay']);
    typeSearch('kontakt');
    expect(groups()).toHaveLength(0); // Guitar on hides instruments
    expect(modal().textContent).toContain('No plugins match');
  });

  it('while searching, matching groups are expanded even if collapsed by default', async () => {
    await open();
    guitarToggle().click();
    typeSearch('kontakt');
    expect(rowNames('instrument')).toEqual(['Kontakt 8']);
    typeSearch('');
    expect(rowNames('instrument')).toEqual([]);
  });

  it('Enter in the search field adds the first visible match', async () => {
    await open();
    typeSearch('rig');
    key(search(), 'Enter');
    expect(lastCommand()).toMatchObject({ cmd: 'addNode', args: { type: 'Plugin', index: 3 } });
    expect(lastCommand().args.plugin).toEqual({ format: 'VST3', uid: 'VST3-Guitar Rig 7', name: 'Guitar Rig 7', vendor: 'Native Instruments' });
    expect(document.querySelector('.modal')).toBeNull();
  });

  it('Enter with no match does nothing', async () => {
    await open();
    typeSearch('zzz');
    key(search(), 'Enter');
    expect(lastCommand()).toMatchObject({ cmd: 'listPlugins' });
    expect(document.querySelector('.modal')).not.toBeNull();
  });

  it('clicking a row adds that plugin and closes the picker', async () => {
    await open();
    visibleRows('reverb')[0].click();
    expect(lastCommand()).toMatchObject({ cmd: 'addNode', args: { type: 'Plugin', index: 3, plugin: { uid: 'VST3-Raum', name: 'Raum' } } });
    expect(document.querySelector('.modal')).toBeNull();
  });

  it('Escape closes the picker, also from the search field', async () => {
    await open();
    key(search(), 'Escape');
    expect(document.querySelector('.modal')).toBeNull();
  });

  it('says when nothing was scanned yet, with a Rescan action', async () => {
    await open([]);
    expect(modal().textContent).toContain('No plugins scanned yet — Rescan');
    const rescan = [...modal().querySelectorAll<HTMLButtonElement>('.plugin-groups button')].find((b) => b.textContent === 'Rescan')!;
    rescan.click();
    await flush();
    const ok = [...document.querySelectorAll<HTMLButtonElement>('.modal button')].find((b) => b.textContent === 'Scan now')!;
    ok.click();
    await flush();
    expect(lastCommand()).toMatchObject({ cmd: 'rescanPlugins' });
    expect(modal().textContent).toContain('Scanning plugins…');
    juce.reply(lastCommand().id, true, { scanning: true }); // the scan runs on; events follow
    await flush();
    expect(modal().textContent).toContain('Scanning plugins…');
    juce.fromBackend('amp', { v: 1, event: 'pluginScanProgress', data: { progress: 0.5, plugin: 'Kontakt 8' } });
    expect(modal().textContent).toContain('Scanning plugins… 50% · Kontakt 8');
    expect([...modal().querySelectorAll<HTMLButtonElement>('button')].find((b) => b.textContent === 'Scanning…')!.disabled).toBe(true);
    juce.fromBackend('amp', { v: 1, event: 'pluginsChanged', data: { plugins: PLUGINS } });
    expect(groupSummary().map(([id]) => id)).toEqual(['drive', 'amps-cabs', 'delay', 'reverb']);
    expect(modal().textContent).not.toContain('Scanning');
  });

  it('follows a scan started elsewhere and stops listening when closed', async () => {
    await open();
    juce.fromBackend('amp', { v: 1, event: 'pluginScanProgress', data: { progress: 0.25, plugin: 'Raum' } });
    expect(modal().textContent).toContain('Scanning plugins… 25% · Raum');
    juce.fromBackend('amp', { v: 1, event: 'pluginsChanged', data: { plugins: PLUGINS.slice(0, 1) } });
    expect(groupSummary()).toEqual([['delay', 'Delay', '1']]);
    key(search(), 'Escape');
    expect(document.querySelector('.modal')).toBeNull();
    juce.fromBackend('amp', { v: 1, event: 'pluginsChanged', data: { plugins: PLUGINS } }); // no modal: nothing to update, no error
  });

  it('shows the engine\'s message when a scan could not run', async () => {
    await open();
    juce.fromBackend('amp', { v: 1, event: 'pluginsChanged', data: { plugins: PLUGINS, error: 'Could not start the plugin scan worker' } });
    expect(document.body.textContent).toContain('Could not start the plugin scan worker');
  });

  it('works with plugin lists from older engines (no category fields)', async () => {
    await open([{ format: 'VST3', uid: 'u', name: 'kHs Reverb', vendor: 'Kilohearts' }]);
    expect(groupSummary()).toEqual([['reverb', 'Reverb', '1']]);
  });
});
