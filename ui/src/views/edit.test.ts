import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { installFakeJuce, type FakeJuce } from '../test-support';

let juce: FakeJuce;
let state: typeof import('../state');
let edit: typeof import('./edit');
let library: typeof import('./library');
let root: HTMLElement;
let unmountOverlay: (() => void) | null = null;

const emptyReport = { missingAssets: [], missingPlugins: [], placeholderNodeIds: [], resolvedByFilename: [] };

function makeRig(overrides: object = {}) {
  return {
    id: 'r1',
    name: 'Black Dog',
    band: 'Zeppelin',
    nodes: [
      { id: 'in', type: 'Input', bypass: false, params: {} },
      { id: 'g', type: 'Gate', bypass: false, params: { threshold: -60, release: 100 } },
      { id: 'amp', type: 'NamAmp', bypass: false, params: { inputGain: 0, outputGain: 0, normalize: 1 }, asset: { kind: 'nam', file: '/m/Plexi.nam', sha256: 'x' } },
      { id: 'cab', type: 'CabIR', bypass: true, params: { lowCut: 80, highCut: 8000, level: 0 }, asset: { kind: 'ir', file: '/i/4x12.wav', sha256: 'y' } },
      { id: 'eq', type: 'EQ', bypass: false, params: { low: 0, mid: 0, high: 0, midFreq: 800 } },
      { id: 'out', type: 'Output', bypass: false, params: {} },
    ],
    connections: [],
    mappings: [
      { control: 'footswitch:L', action: 'toggleBypass', node: 'g' },
      { control: 'midi:Pedal:cc64', action: 'toggleBypass', node: 'eq' },
    ],
    ...overrides,
  };
}

const flush = () => new Promise((r) => setTimeout(r, 0));
const lastCommand = () => juce.commands().at(-1);
const cards = () => [...root.querySelectorAll<HTMLElement>('.card[data-node-id]')];
const card = (id: string) => root.querySelector<HTMLElement>(`.card[data-node-id="${id}"]`)!;
const menuItem = (label: string) =>
  [...document.querySelectorAll<HTMLButtonElement>('.menu [role="menuitem"]')].find((b) => b.textContent === label)!;

async function started(rig: object = makeRig(), report: object = emptyReport) {
  const p = state.initStore();
  const getState = juce.commands().find((c) => c.cmd === 'getState')!;
  juce.reply(getState.id, true, {
    protocol: 1, rig, report, dirty: false, saved: true, learning: false, startupErrors: [], settings: {},
    deviceStatus: { status: 'Running', latencyMs: 8, xruns: 0 }, controlStatus: { hid: 'NotFound', exclusive: false, midiDevices: [] },
    tunerOn: false, favorites: [], recents: [], bands: ['Zeppelin'],
  });
  await p;
}

function pointer(type: string, target: EventTarget, x: number, y = 10) {
  target.dispatchEvent(new PointerEvent(type, { bubbles: true, clientX: x, clientY: y, button: 0, pointerId: 1 }));
}

beforeEach(async () => {
  vi.resetModules();
  document.body.innerHTML = '';
  juce = installFakeJuce();
  state = await import('../state');
  edit = await import('./edit');
  library = await import('./library');
  root = document.createElement('div');
  document.body.append(root);
});

afterEach(() => {
  unmountOverlay?.();
  unmountOverlay = null;
  document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape' }));
  delete (window as any).__JUCE__;
});

describe('pedalboard', () => {
  it('shows an art thumbnail on each card and offers the built-in art per kind', async () => {
    const art = await import('../art/archetypes');
    await started();
    edit.renderEdit(root);
    expect(card('amp').querySelector('img.card-art')!.getAttribute('src')).toBe(art.archetypeUrl('british-stack')); // "Plexi"
    expect(card('cab').querySelector('img.card-art')!.getAttribute('src')).toBe(art.archetypeUrl('cab-4x12'));
    expect(card('g').querySelector('img.card-art')!.getAttribute('src')).toBe(art.archetypeUrl('gate'));

    card('cab').dispatchEvent(new MouseEvent('contextmenu', { bubbles: true, clientX: 5, clientY: 5 }));
    menuItem('Set art…').click();
    const options = [...document.querySelectorAll<HTMLOptionElement>('.modal select option')].map((o) => o.value);
    expect(options).toEqual(['', ...art.ARCHETYPES.cab]);
  });

  it('renders nodes in chain order excluding in/out', async () => {
    await started();
    edit.renderEdit(root);
    expect(cards().map((c) => c.dataset.nodeId)).toEqual(['g', 'amp', 'cab', 'eq']);
    expect(card('amp').classList.contains('card-amp')).toBe(true);
    expect(card('cab').classList.contains('card-cab')).toBe(true);
    expect(card('amp').textContent).toContain('Plexi');
    expect(root.querySelector('.card-add')).not.toBeNull();
  });

  it('bypassed node has .off class', async () => {
    await started();
    edit.renderEdit(root);
    expect(card('cab').classList.contains('off')).toBe(true);
    expect(card('g').classList.contains('off')).toBe(false);

    card('g').querySelector<HTMLButtonElement>('.led')!.click();
    expect(lastCommand()).toMatchObject({ cmd: 'toggleBypass', args: { id: 'g' } });
    juce.fromBackend('amp', { v: 1, event: 'bypassChanged', data: { id: 'g', bypass: true } });
    expect(card('g').classList.contains('off')).toBe(true);
  });

  it('dragging card from index 0 to 2 calls moveNode{id,index:2}', async () => {
    await started();
    edit.renderEdit(root);
    cards().forEach((c, i) => {
      c.getBoundingClientRect = () => ({ left: i * 110, right: i * 110 + 100, width: 100, top: 0, bottom: 100, height: 100, x: i * 110, y: 0, toJSON() {} }) as DOMRect;
    });
    const gate = card('g');
    pointer('pointerdown', gate.querySelector('.card-title')!, 50);
    pointer('pointermove', window, 150);
    pointer('pointermove', window, 280); // past the midpoints of cards 1 (165) and 2 (275)
    pointer('pointerup', window, 280);
    expect(lastCommand()).toMatchObject({ cmd: 'moveNode', args: { id: 'g', index: 2 } });
  });

  it('a click without movement expands the card instead of moving it', async () => {
    await started();
    edit.renderEdit(root);
    const gate = card('g');
    pointer('pointerdown', gate.querySelector('.card-title')!, 50);
    pointer('pointerup', window, 51);
    expect(juce.commands().some((c) => c.cmd === 'moveNode')).toBe(false);
    expect(card('g').classList.contains('expanded')).toBe(true);
    expect([...card('g').querySelectorAll('[role="slider"]')].map((k) => k.getAttribute('aria-label'))).toEqual(['Threshold', 'Release']);
  });

  it('right-click → Learn footswitch calls learnMapping', async () => {
    await started();
    edit.renderEdit(root);
    unmountOverlay = edit.mountLearnOverlay(root);
    card('amp').dispatchEvent(new MouseEvent('contextmenu', { bubbles: true, clientX: 10, clientY: 10 }));
    menuItem('Learn footswitch').click();
    expect(lastCommand()).toMatchObject({ cmd: 'learnMapping', args: { id: 'amp' } });
    juce.reply(lastCommand().id, true, {});
    await flush();
    expect(root.querySelector('.learn-overlay')?.textContent).toContain('Press a footswitch');

    document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape' }));
    expect(lastCommand()).toMatchObject({ cmd: 'cancelLearn' });
    juce.reply(lastCommand().id, true, {});
    await flush();
    expect(root.querySelector('.learn-overlay')).toBeNull();
  });

  it('learned event closes the overlay and labels the node with its footswitch', async () => {
    await started();
    edit.renderEdit(root);
    unmountOverlay = edit.mountLearnOverlay(root);
    card('amp').querySelector<HTMLButtonElement>('.card-menu-button')!.click();
    menuItem('Learn footswitch').click();
    juce.reply(lastCommand().id, true, {});
    await flush();
    juce.fromBackend('amp', { v: 1, event: 'learned', data: { control: 'footswitch:L', node: 'amp' } });
    expect(root.querySelector('.learn-overlay')).toBeNull();
    expect(card('amp').querySelector('.fs-label')?.textContent).toBe('L');
    expect(card('g').querySelector('.fs-label')).toBeNull(); // footswitch:L moved from the gate to the amp
  });

  it('shows footswitch labels under mapped nodes', async () => {
    await started();
    edit.renderEdit(root);
    expect(card('g').querySelector('.fs-label')?.textContent).toBe('L');
    expect(card('eq').querySelector('.fs-label')?.textContent).toBe('MIDI cc64');
    expect(card('amp').querySelector('.fs-label')).toBeNull();
  });

  it('a knob drag cut short by a rig change sends nothing for the old node', async () => {
    await started();
    edit.renderEdit(root);
    pointer('pointerdown', card('g').querySelector('.card-title')!, 50);
    pointer('pointerup', window, 50);
    const knob = card('g').querySelector<HTMLElement>('[role="slider"]')!;
    pointer('pointerdown', knob, 0, 300);
    pointer('pointermove', window, 0, 290);
    expect(lastCommand()).toMatchObject({ cmd: 'setParam', args: { id: 'g', name: 'threshold' } });
    const sent = juce.commands().length;

    // e.g. a footswitch press loads another rig mid-drag
    const other = makeRig({ id: 'r2', name: 'Kashmir' });
    other.nodes = other.nodes.filter((n) => n.id !== 'g');
    juce.fromBackend('amp', { v: 1, event: 'rigLoaded', data: { rig: other, report: emptyReport, saved: true } });
    for (let y = 280; y > 200; y -= 10) pointer('pointermove', window, 0, y);
    pointer('pointerup', window, 0, 200);
    await new Promise((r) => setTimeout(r, 60)); // past the throttle window: a trailing call would have fired
    expect(juce.commands().slice(sent).filter((c) => c.cmd === 'setParam')).toEqual([]);
    expect(document.querySelector('.toast')).toBeNull();
  });

  it('placeholder node shows "missing" badge with original name', async () => {
    const rig = makeRig();
    rig.nodes.splice(4, 0, { id: 'p', type: 'Plugin', bypass: true, params: {}, plugin: { format: 'VST3', uid: 'u', name: 'Space Echo X', vendor: 'V' } } as any);
    await started(rig, { ...emptyReport, missingPlugins: ['Space Echo X'], placeholderNodeIds: ['p'] });
    edit.renderEdit(root);
    expect(card('p').classList.contains('missing')).toBe(true);
    expect(card('p').querySelector('.badge-missing')?.textContent).toBe('missing');
    expect(card('p').textContent).toContain('Space Echo X');
    expect(card('p').querySelector('.led')!.hasAttribute('disabled')).toBe(true);
    pointer('pointerdown', card('p').querySelector('.card-title')!, 50);
    pointer('pointerup', window, 50);
    expect(card('p').querySelectorAll('[role="slider"]').length).toBe(0);
  });

  it('adding a NAM amp opens the model picker and assigns the chosen model', async () => {
    await started();
    edit.renderEdit(root);
    root.querySelector<HTMLButtonElement>('.card-add')!.click();
    menuItem('NAM Amp').click();
    expect(lastCommand()).toMatchObject({ cmd: 'addNode', args: { type: 'NamAmp', index: 4 } });
    juce.reply(lastCommand().id, true, { id: 'new1' });
    await flush();
    expect(lastCommand()).toMatchObject({ cmd: 'listAssets', args: { kind: 'nam' } });
    juce.reply(lastCommand().id, true, { assets: [{ file: '/m/JCM.nam', name: 'JCM', kind: 'nam', sha256: 'z' }] });
    await flush();
    [...document.querySelectorAll<HTMLButtonElement>('.asset-list button')].find((b) => b.textContent!.includes('JCM'))!.click();
    expect(lastCommand()).toMatchObject({ cmd: 'setAsset', args: { id: 'new1', file: '/m/JCM.nam' } });
  });
});

describe('minor fixes', () => {
  it('a missing model offers Locate… which assigns a file from the library', async () => {
    await started(makeRig(), { ...emptyReport, missingAssets: ['/m/Plexi.nam'], placeholderNodeIds: ['amp'] });
    edit.renderEdit(root);
    const locate = [...card('amp').querySelectorAll<HTMLButtonElement>('button')].find((b) => b.textContent === 'Locate…')!;
    expect(locate).toBeDefined();
    locate.click();
    expect(lastCommand()).toMatchObject({ cmd: 'listAssets', args: { kind: 'nam' } });
    juce.reply(lastCommand().id, true, { assets: [{ file: '/new/Plexi.nam', name: 'Plexi', kind: 'nam', sha256: 'q' }] });
    await flush();
    [...document.querySelectorAll<HTMLButtonElement>('.asset-list button')].find((b) => b.textContent!.includes('Plexi'))!.click();
    expect(lastCommand()).toMatchObject({ cmd: 'setAsset', args: { id: 'amp', file: '/new/Plexi.nam' } });

    document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape' }));
    card('amp').dispatchEvent(new MouseEvent('contextmenu', { bubbles: true, clientX: 5, clientY: 5 }));
    expect(menuItem('Locate…')).toBeDefined();
  });

  it('the card menu clears a footswitch mapping', async () => {
    await started();
    edit.renderEdit(root);
    card('g').dispatchEvent(new MouseEvent('contextmenu', { bubbles: true, clientX: 5, clientY: 5 }));
    menuItem('Clear footswitch L').click();
    expect(lastCommand()).toMatchObject({ cmd: 'clearMapping', args: { control: 'footswitch:L' } });
    juce.reply(lastCommand().id, true, {});
    juce.fromBackend('amp', { v: 1, event: 'mappingCleared', data: { control: 'footswitch:L' } });
    await flush();
    expect(card('g').querySelector('.fs-label')).toBeNull();
    expect(card('eq').querySelector('.fs-label')!.textContent).toContain('MIDI cc64');
  });

  it('learnCancelled closes the learn overlay', async () => {
    await started();
    edit.renderEdit(root);
    unmountOverlay = edit.mountLearnOverlay(document.body);
    card('g').dispatchEvent(new MouseEvent('contextmenu', { bubbles: true, clientX: 5, clientY: 5 }));
    menuItem('Learn footswitch').click();
    juce.reply(lastCommand().id, true, {});
    await flush();
    expect(document.querySelector('.learn-overlay')).not.toBeNull();
    juce.fromBackend('amp', { v: 1, event: 'learnCancelled', data: {} });
    expect(document.querySelector('.learn-overlay')).toBeNull();
  });

  it('the master level knob sets the Output node level', async () => {
    const rig = makeRig();
    rig.nodes[5] = { id: 'out', type: 'Output', bypass: false, params: { level: -3 } } as any;
    await started(rig);
    edit.renderEdit(root);
    const knob = root.querySelector<HTMLElement>('.master-level [role="slider"]')!;
    expect(knob).not.toBeNull();
    expect(knob.getAttribute('aria-valuenow')).toBe('-3');
    knob.dispatchEvent(new MouseEvent('dblclick', { bubbles: true })); // reset to 0 dB
    expect(lastCommand()).toMatchObject({ cmd: 'setParam', args: { id: 'out', name: 'level', value: 0 } });
    juce.fromBackend('amp', { v: 1, event: 'paramChanged', data: { id: 'out', name: 'level', value: -9 } });
    expect(knob.getAttribute('aria-valuenow')).toBe('-9');
  });
});

describe('knob', () => {
  it('sends setParam throttled, always delivering the last value', async () => {
    vi.useFakeTimers();
    try {
      const { throttle } = await import('../components/knob');
      const sent: number[] = [];
      const t = throttle((v: number) => sent.push(v), 33);
      t(1);
      t(2);
      t(3);
      expect(sent).toEqual([1]);
      vi.advanceTimersByTime(40);
      expect(sent).toEqual([1, 3]);
    } finally {
      vi.useRealTimers();
    }
  });

  it('drags vertically, resets on double-click and exposes aria values', async () => {
    const { createKnob } = await import('../components/knob');
    const changes: number[] = [];
    const k = createKnob({ label: 'Level', min: -24, max: 12, value: 0, defaultValue: 0, unit: 'dB', onChange: (v) => changes.push(v) });
    document.body.append(k.el);
    expect(k.el.getAttribute('aria-valuenow')).toBe('0');
    pointer('pointerdown', k.el, 0, 100);
    pointer('pointermove', window, 0, 50); // up = louder
    pointer('pointerup', window, 0, 50);
    expect(changes.at(-1)!).toBeGreaterThan(0);
    k.el.dispatchEvent(new MouseEvent('dblclick', { bubbles: true }));
    expect(changes.at(-1)).toBe(0);
    expect(k.el.textContent).toContain('dB');
  });
});

describe('rig tags', () => {
  const chips = () => [...root.querySelectorAll('.edit-tags .tag-chip .tag-text')].map((c) => c.textContent);
  const tagInput = () => root.querySelector<HTMLInputElement>('.edit-tags .tag-input')!;
  function addTag(text: string) {
    tagInput().value = text;
    tagInput().dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true, cancelable: true }));
  }

  it('shows the rig tags above the pedalboard and follows tagsChanged and rig loads', async () => {
    await started(makeRig({ tags: ['70s', 'Led Zeppelin'] }));
    edit.renderEdit(root);
    expect(root.querySelector('.edit-tags')!.compareDocumentPosition(root.querySelector('.pedalboard')!) & Node.DOCUMENT_POSITION_FOLLOWING).toBeTruthy();
    expect(chips()).toEqual(['70s', 'Led Zeppelin']);
    juce.fromBackend('amp', { v: 1, event: 'tagsChanged', data: { tags: ['70s'] } });
    expect(chips()).toEqual(['70s']);
    juce.fromBackend('amp', { v: 1, event: 'rigLoaded', data: { rig: makeRig({ id: 'r2', tags: undefined }), report: emptyReport, saved: true } });
    expect(chips()).toEqual([]);
  });

  it('every change calls setTags with the full list', async () => {
    await started(makeRig({ tags: ['70s'] }));
    edit.renderEdit(root);
    addTag('Parkway Drive');
    expect(lastCommand()).toMatchObject({ cmd: 'setTags', args: { tags: ['70s', 'Parkway Drive'] } });
    juce.reply(lastCommand().id, true, { tags: ['70s', 'Parkway Drive'] });
    root.querySelector<HTMLButtonElement>('.edit-tags .tag-remove')!.click();
    expect(lastCommand()).toMatchObject({ cmd: 'setTags', args: { tags: ['Parkway Drive'] } });
  });

  it('a rejected setTags shows why, puts the rig tags back and the typed text back in the input', async () => {
    await started(makeRig({ tags: ['70s'] }));
    edit.renderEdit(root);
    addTag('Wild Eyes');
    expect(chips()).toEqual(['70s', 'Wild Eyes']);
    expect(tagInput().value).toBe('');
    juce.reply(lastCommand().id, false, 'too many tags: 33 (at most 32)');
    await flush();
    expect(chips()).toEqual(['70s']);
    expect(tagInput().value).toBe('Wild Eyes');
    expect(document.querySelector('.toast')!.textContent).toContain('too many tags');
  });

  it('suggests tags from listTags while typing', async () => {
    await started(makeRig({ tags: [] }));
    edit.renderEdit(root);
    tagInput().focus();
    expect(lastCommand()).toMatchObject({ cmd: 'listTags' });
    juce.reply(lastCommand().id, true, { tags: [{ tag: 'Parkway Drive', count: 3 }, { tag: 'metal', count: 2 }] });
    await flush();
    tagInput().value = 'park';
    tagInput().dispatchEvent(new Event('input', { bubbles: true }));
    expect([...root.querySelectorAll('.tag-suggestion')].map((s) => s.textContent)).toEqual(['Parkway Drive']);
  });
});

describe('library', () => {
  it('library groups rigs by band and calls loadRig on click', async () => {
    await started();
    library.renderLibrary(root);
    const list = juce.commands().find((c) => c.cmd === 'listRigs')!;
    juce.reply(list.id, true, {
      rigs: [
        { id: 'a', name: 'Plush', band: 'STP', favorite: false },
        { id: 'r1', name: 'Black Dog', band: 'Zeppelin', favorite: true },
        { id: 'b', name: 'Kashmir', band: 'Zeppelin', favorite: false },
      ],
      bands: ['STP', 'Zeppelin'],
    });
    await flush();
    const groups = [...root.querySelectorAll('.band-group')].map((g) => ({
      band: g.querySelector('.band-name')!.textContent,
      rigs: [...g.querySelectorAll('.rig-load')].map((b) => b.textContent),
    }));
    expect(groups).toEqual([
      { band: 'STP', rigs: ['Plush'] },
      { band: 'Zeppelin', rigs: ['Black Dog', 'Kashmir'] },
    ]);
    [...root.querySelectorAll<HTMLButtonElement>('.band-group .rig-load')].find((b) => b.textContent === 'Kashmir')!.click();
    await flush();
    expect(lastCommand()).toMatchObject({ cmd: 'loadRig', args: { id: 'b' } });
  });

  it('a failed loadRig toast offers to open the last backup', async () => {
    await started();
    library.renderLibrary(root);
    juce.reply(juce.commands().find((c) => c.cmd === 'listRigs')!.id, true, {
      rigs: [{ id: 'bad', name: 'Broken', band: 'X', favorite: false }],
      bands: ['X'],
    });
    await flush();
    root.querySelector<HTMLButtonElement>('.band-group .rig-load')!.click();
    await flush();
    juce.reply(lastCommand().id, false, 'parse error at line 1');
    await flush();
    const backup = [...document.querySelectorAll<HTMLButtonElement>('.toast button')].find((b) => b.textContent === 'Open last backup')!;
    expect(backup).toBeDefined();
    backup.click();
    await flush();
    expect(lastCommand()).toMatchObject({ cmd: 'restoreBackup', args: { id: 'bad' } });
  });

  it('stars toggle favorites and the online tab is disabled', async () => {
    await started();
    library.renderLibrary(root);
    juce.reply(juce.commands().find((c) => c.cmd === 'listRigs')!.id, true, {
      rigs: [{ id: 'a', name: 'Plush', band: 'STP', favorite: false }],
      bands: ['STP'],
    });
    await flush();
    root.querySelector<HTMLButtonElement>('.band-group .star')!.click();
    expect(lastCommand()).toMatchObject({ cmd: 'toggleFavorite', args: { id: 'a' } });
    const online = [...root.querySelectorAll<HTMLButtonElement>('[role="tab"]')].find((t) => t.textContent!.includes('Search online'))!;
    expect(online.disabled).toBe(true);
    expect(online.textContent).toContain('coming soon');
  });

  it('deleting a rig asks first, then calls deleteRig', async () => {
    await started();
    library.renderLibrary(root);
    juce.reply(juce.commands().find((c) => c.cmd === 'listRigs')!.id, true, {
      rigs: [{ id: 'a', name: 'Plush', band: 'STP', favorite: false }],
      bands: ['STP'],
    });
    await flush();
    root.querySelector<HTMLButtonElement>('.band-group .rig-delete')!.click();
    expect(document.querySelector('.modal')!.textContent).toContain('Plush');
    expect(juce.commands().some((c) => c.cmd === 'deleteRig')).toBe(false);
    [...document.querySelectorAll<HTMLButtonElement>('.modal button')].find((b) => b.textContent === 'Move to Trash')!.click();
    await flush();
    expect(lastCommand()).toMatchObject({ cmd: 'deleteRig', args: { id: 'a' } });
  });
});
