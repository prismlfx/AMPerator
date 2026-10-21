// Plugin picker groups: sorts scanned plugins into guitar-oriented groups from what the scan reports (instrument flag,
// declared category, channel counts) and keywords in the plugin's name (pure; used by the plugin picker).

import { queryWords } from './rigFilter';

/** A plugin as listPlugins / rescanPlugins report it (PluginInfo in docs/bridge-protocol.md). The scan fields are
    missing in replies of older engines. */
export interface PluginInfo {
  format: string;
  uid: string;
  name: string;
  vendor: string;
  category?: string; // juce::PluginDescription::category verbatim: "Fx|Distortion", "Instrument|Synth", AU "Effect"...
  isInstrument?: boolean;
  numInputChannels?: number;
  numOutputChannels?: number;
}

export type GroupId =
  | 'drive'
  | 'amps-cabs'
  | 'modulation'
  | 'delay'
  | 'reverb'
  | 'dynamics'
  | 'eq'
  | 'filter-wah'
  | 'pitch'
  | 'utility'
  | 'instrument';

export interface Group {
  id: GroupId;
  label: string;
  /** Whether the group is useful in a guitar chain (the picker's Guitar toggle shows only these). */
  guitar: boolean;
}

/** Every group, in display order. */
export const GROUPS: readonly Group[] = [
  { id: 'drive', label: 'Drive', guitar: true },
  { id: 'amps-cabs', label: 'Amps & Cabs', guitar: true },
  { id: 'modulation', label: 'Modulation', guitar: true },
  { id: 'delay', label: 'Delay', guitar: true },
  { id: 'reverb', label: 'Reverb', guitar: true },
  { id: 'dynamics', label: 'Dynamics', guitar: true },
  { id: 'eq', label: 'EQ', guitar: true },
  { id: 'filter-wah', label: 'Filter & Wah', guitar: true },
  { id: 'pitch', label: 'Pitch', guitar: true },
  { id: 'utility', label: 'Utility & Other', guitar: false },
  { id: 'instrument', label: 'Instruments', guitar: false },
];

const ORDER = new Map(GROUPS.map((g, i) => [g.id, i]));
const GUITAR = new Set(GROUPS.filter((g) => g.guitar).map((g) => g.id));

/** Words in a declared category (any part) that mean an instrument. */
const INSTRUMENT_CATEGORY = ['instrument', 'synth', 'sampler', 'drum', 'generator'];

/** Declared category, last "|" part (case-insensitive substring) → group. */
const CATEGORY_RULES: [string[], GroupId][] = [
  [['distortion'], 'drive'],
  [['delay'], 'delay'],
  [['reverb'], 'reverb'],
  [['modulation'], 'modulation'],
  [['dynamics'], 'dynamics'],
  [['eq'], 'eq'],
  [['filter'], 'filter-wah'],
  [['pitch'], 'pitch'],
  [['spatial', 'tools', 'analyzer', 'restoration', 'mastering', 'network', 'surround', 'utility'], 'utility'],
];

/** Name keywords → group. A keyword matches at the start of a word of the name ("comp" → "Compressor", "MCompressor";
    not "Sampler" for "amp"). `=word` matches a whole word only; `~word` anywhere inside a word ("Bitcrush"). Words
    come from spaces, punctuation and CamelCase ("MAutoPitch" → m auto pitch); a run-together name also counts as one
    word ("LiveCut" → livecut). Multi-word keywords match consecutive words. */
const NAME_RULES: [string[], GroupId][] = [
  [
    ['drive', 'dist', 'dirt', 'bite', 'screamer', 'fuzz', '~crush', 'saturat', 'shaper', 'wavefold', 'tube', 'overdrive',
      '=od', '=rat', 'klon', 'muff'],
    'drive',
  ],
  [['amp', 'rig', 'cab', 'neural', '=nam', 'helix', 'archetype', 'tonex', 'ir loader'], 'amps-cabs'],
  [
    ['chorus', 'flanger', 'phaser', 'phase', 'vibe', 'vibrato', 'tremolo', 'trem', 'rotary', 'leslie', 'ensemble', 'choral',
      'flair', 'phasis', 'freak', 'ring mod', 'uni-v'],
    'modulation',
  ],
  [['delay', 'echo', 'replika', 'tape stop', 'reverser'], 'delay'],
  [['reverb', '~verb', 'raum', 'hall', 'plate', 'spring', 'shimmer', 'supermassive', 'space'], 'reverb'],
  [['comp', 'limit', 'gate', 'transient', 'dynamics', 'expander', 'bus comp', 'supercharger'], 'dynamics'],
  [['eq', 'equal'], 'eq'],
  [['wah', 'filter', 'formant', 'resonator', 'comb'], 'filter-wah'],
  [['pitch', 'octav', 'harmon', 'whammy', 'tuner', 'autopitch', 'graillon', 'shifter', 'vocoder'], 'pitch'],
  [
    ['analyzer', 'meter', 'loudness', 'span', 'utility', 'gain', 'mixer', 'stereo', 'wider', 'haas', 'channel', 'codec',
      'vinyl', 'ozone', 'kickstart', 'livecut', 'sweep', 'midi', 'chords', 'pilot', 'captain', 'human'],
    'utility',
  ],
];

/** Lower-case words of a plugin name: split at spaces, punctuation and CamelCase. */
function nameWords(name: string): string[] {
  return name
    .replace(/([a-z0-9])([A-Z])/g, '$1 $2') // camelCase → camel Case
    .replace(/([A-Z]+)([A-Z][a-z])/g, '$1 $2') // MSaturator → M Saturator
    .toLowerCase()
    .split(/[^a-z0-9]+/)
    .filter((w) => w.length > 0);
}

/** The name's words, plus each space-separated part with its CamelCase run together ("LiveCut" → livecut). */
function wordSets(name: string): string[][] {
  const runTogether = name
    .toLowerCase()
    .split(/\s+/)
    .map((w) => w.replace(/[^a-z0-9]+/g, ''))
    .filter((w) => w.length > 0);
  return [nameWords(name), runTogether];
}

function keywordMatches(keyword: string, sets: string[][]): boolean {
  const mode = keyword[0] === '=' || keyword[0] === '~' ? keyword[0] : '';
  const parts = nameWords(mode ? keyword.slice(1) : keyword);
  return sets.some((words) =>
    words.some((_, i) =>
      parts.every((part, j) => {
        const w = words[i + j];
        if (w === undefined) return false;
        if (mode === '=') return w === part;
        if (mode === '~') return w.includes(part);
        // Earlier parts of a multi-word keyword must be whole words ("ring mod" → "Ring Modulator", not "Ringo Mod").
        return j < parts.length - 1 ? w === part : w.startsWith(part);
      }),
    ),
  );
}

function isInstrument(p: PluginInfo): boolean {
  const category = (p.category ?? '').toLowerCase();
  return p.isInstrument === true || INSTRUMENT_CATEGORY.some((c) => category.includes(c)) || p.numInputChannels === 0;
}

function byDisplayOrder(groups: Set<GroupId>): GroupId[] {
  return [...groups].sort((a, b) => ORDER.get(a)! - ORDER.get(b)!);
}

/** The groups `p` belongs to, in display order (never empty). Instruments are only `instrument` (plus `amps-cabs` if
    the name says so); effects get every group their declared category and name keywords point to, else `utility`. */
export function categorize(p: PluginInfo): GroupId[] {
  const sets = wordSets(p.name);
  const nameSays = (g: GroupId) => NAME_RULES.some(([words, group]) => group === g && words.some((k) => keywordMatches(k, sets)));

  if (isInstrument(p)) return nameSays('amps-cabs') ? ['amps-cabs', 'instrument'] : ['instrument'];

  const groups = new Set<GroupId>();
  const declared = (p.category ?? '').split('|').pop()!.toLowerCase();
  if (declared)
    for (const [words, group] of CATEGORY_RULES) if (words.some((w) => declared.includes(w))) groups.add(group);
  for (const [words, group] of NAME_RULES) if (words.some((k) => keywordMatches(k, sets))) groups.add(group);
  if (groups.size === 0) groups.add('utility');
  return byDisplayOrder(groups);
}

/** Whether `p` is in at least one guitar group. */
export function isGuitarPlugin(p: PluginInfo): boolean {
  return categorize(p).some((g) => GUITAR.has(g));
}

/** The plugins, in their order, matching every word of `query` (case-insensitive substring of the name or the vendor);
    with `guitarOnly`, without the plugins that are only utility / instrument. */
export function filterPlugins<T extends PluginInfo>(list: readonly T[], query: string, guitarOnly: boolean): T[] {
  const words = queryWords(query);
  return list.filter((p) => {
    if (guitarOnly && !isGuitarPlugin(p)) return false;
    const fields = [p.name, p.vendor].map((f) => (f ?? '').toLowerCase());
    return words.every((w) => fields.some((f) => f.includes(w)));
  });
}

/** The plugins sorted into their groups (a plugin appears in each of its groups): groups in display order, empty
    groups left out, plugins by name (then format) within a group. */
export function groupPlugins<T extends PluginInfo>(list: readonly T[]): { group: Group; plugins: T[] }[] {
  const byGroup = new Map<GroupId, T[]>();
  for (const p of list) for (const g of categorize(p)) byGroup.set(g, [...(byGroup.get(g) ?? []), p]);
  const compare = (a: T, b: T) =>
    a.name.localeCompare(b.name, undefined, { sensitivity: 'base' }) || a.format.localeCompare(b.format);
  return GROUPS.filter((g) => byGroup.has(g.id)).map((group) => ({ group, plugins: byGroup.get(group.id)!.sort(compare) }));
}
