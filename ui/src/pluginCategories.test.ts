import { describe, expect, it } from 'vitest';
import { GROUPS, categorize, filterPlugins, groupPlugins, type GroupId, type PluginInfo } from './pluginCategories';

/** An effect plugin as listPlugins reports it (stereo in/out, AU "Effect" unless overridden). */
function fx(name: string, more: Partial<PluginInfo> = {}): PluginInfo {
  return {
    format: 'AudioUnit',
    uid: `uid-${name}`,
    name,
    vendor: 'Vendor',
    category: 'Effect',
    isInstrument: false,
    numInputChannels: 2,
    numOutputChannels: 2,
    ...more,
  };
}
const synth = (name: string, more: Partial<PluginInfo> = {}) =>
  fx(name, { category: 'Instrument|Synth', isInstrument: true, numInputChannels: 0, ...more });

describe('categorize: the user’s plugins by name (AU category "Effect" says nothing)', () => {
  const cases: [string, GroupId[]][] = [
    // NI effects
    ['Dirt', ['drive']],
    ['Driver', ['drive']],
    ['Bite', ['drive']],
    ['Choral', ['modulation']],
    ['Flair', ['modulation']],
    ['Phasis', ['modulation']],
    ['Freak', ['modulation']],
    ['Raum', ['reverb']],
    ['Replika', ['delay']],
    ['Replika XT', ['delay']],
    ['Supercharger', ['dynamics']], // a tube compressor, not a drive
    ['Supercharger GT', ['dynamics']],
    ['Transient Master', ['dynamics']],
    ['Solid Bus Comp', ['dynamics']],
    ['Solid Dynamics', ['dynamics']],
    ['Solid EQ', ['eq']],
    ['Guitar Rig 7', ['amps-cabs']],
    // Kilohearts
    ['kHs Distortion', ['drive']],
    ['kHs Delay', ['delay']],
    ['kHs Reverb', ['reverb']],
    ['kHs Chorus', ['modulation']],
    ['kHs Phaser', ['modulation']],
    ['kHs Compressor', ['dynamics']],
    ['kHs Gate', ['dynamics']],
    ['kHs Trance Gate', ['dynamics']], // the word "gate"
    ['kHs Pitch Shifter', ['pitch']],
    ['kHs Ladder Filter', ['filter-wah']],
    ['kHs Bitcrush', ['drive']],
    ['kHs Gain', ['utility']],
    ['kHs Haas', ['utility']],
    // MeldaProduction (one CamelCase word)
    ['MSaturator', ['drive']],
    ['MDelay', ['delay']],
    ['MCharmVerb', ['reverb']],
    ['MCompressor', ['dynamics']],
    ['MEqualizer', ['eq']],
    ['MFlanger', ['modulation']],
    ['MPhaser', ['modulation']],
    ['MTremolo', ['modulation']],
    ['MVibrato', ['modulation']],
    ['MTuner', ['pitch']],
    ['MAutoPitch', ['pitch']],
    ['MAnalyzer', ['utility']],
    ['MUtility', ['utility']],
    ['MOscillator', ['utility']], // nothing matched: "Other"
    // Valhalla (AU names run the words together)
    ['ValhallaFreqEcho', ['delay']], // "Freq" is not "eq", "Valhalla" is not "hall"
    ['ValhallaSupermassive', ['reverb']],
    ['ValhallaSpaceModulator', ['reverb']], // "space"
    // the rest
    ['NeuralAmpModeler', ['amps-cabs']],
    ['Ozone 11', ['utility']],
    ['SPAN', ['utility']],
    ['Youlean Loudness Meter 2', ['utility']],
    ['Kickstart 2', ['utility']],
    ['LiveCut', ['utility']],
    ['Wider', ['utility']],
    ['Vinyl', ['utility']],
    ['codec', ['utility']],
    ['sweep', ['utility']],
    ['Captain Chords', ['utility']],
    ['Captain Pilot', ['utility']],
    ['Graillon 3', ['pitch']],
    ['Human MIDI', ['utility']],
    ['Mixed In Key SE', ['utility']],
    ['Transperc', ['utility']],
    ['KClip Zero', ['utility']],
  ];

  it.each(cases)('%s → %j', (name, groups) => {
    expect(categorize(fx(name))).toEqual(groups);
  });

  it('covers at least 40 real plugin names', () => {
    expect(cases.length).toBeGreaterThanOrEqual(40);
  });
});

describe('categorize: instruments', () => {
  it.each([
    ['Kontakt 8', synth('Kontakt 8', { category: 'Instrument|Sampler' })],
    ['Battery 4', synth('Battery 4', { category: 'Instrument|Drum' })],
    ['Massive', synth('Massive')],
    ['Pigments', synth('Pigments')],
    ['ARP 2600 V3', synth('ARP 2600 V3')],
    ['Analog Lab V', synth('Analog Lab V')],
  ])('%s → instrument only', (_, p) => {
    expect(categorize(p)).toEqual(['instrument']);
  });

  it('isInstrument alone, an instrument-like category alone or zero audio inputs alone is enough', () => {
    expect(categorize(fx('Reaktor 6', { isInstrument: true }))).toEqual(['instrument']);
    expect(categorize(fx('Thing', { category: 'Synth' }))).toEqual(['instrument']); // AU
    expect(categorize(fx('Thing', { category: 'Generator' }))).toEqual(['instrument']); // AU
    expect(categorize(fx('MNoiseGenerator', { category: 'Generator' }))).toEqual(['instrument']);
    expect(categorize(fx('Rolling Sampler', { category: 'Instrument|Sampler' }))).toEqual(['instrument']);
    expect(categorize(fx('Thing', { category: 'Fx|Drum' }))).toEqual(['instrument']);
    expect(categorize(fx('Thing', { numInputChannels: 0 }))).toEqual(['instrument']);
  });

  it('an instrument named like an effect stays an instrument (only an amp/cab name adds a group)', () => {
    expect(categorize(synth('Tape Delay Synth'))).toEqual(['instrument']);
    expect(categorize(synth('Amp Sim Instrument'))).toEqual(['amps-cabs', 'instrument']);
  });

  it('an AU "MusicEffect" is an effect, and a missing channel count does not make an instrument', () => {
    expect(categorize(fx('kHs Delay', { category: 'MusicEffect' }))).toEqual(['delay']);
    expect(categorize({ format: 'VST3', uid: 'u', name: 'kHs Reverb', vendor: 'Kilohearts' })).toEqual(['reverb']);
  });
});

describe('categorize: declared categories', () => {
  it.each<[string, GroupId[]]>([
    ['Fx|Distortion', ['drive']],
    ['Fx|Delay', ['delay']],
    ['Fx|Reverb', ['reverb']],
    ['Fx|Modulation', ['modulation']],
    ['Fx|Dynamics', ['dynamics']],
    ['Fx|EQ', ['eq']],
    ['Fx|Filter', ['filter-wah']],
    ['Fx|Pitch Shift', ['pitch']],
    ['Fx|Spatial', ['utility']],
    ['Fx|Tools', ['utility']],
    ['Fx|Analyzer', ['utility']],
    ['Fx|Restoration', ['utility']],
    ['Fx|Mastering', ['utility']],
    ['Fx|Network', ['utility']],
    ['Fx|Surround', ['utility']],
    ['Fx|Utility', ['utility']],
    ['fx|distortion', ['drive']], // case-insensitive
  ])('%s → %j', (category, groups) => {
    expect(categorize(fx('Plain Name', { format: 'VST3', category }))).toEqual(groups);
  });

  it('only the part after the last "|" counts', () => {
    expect(categorize(fx('Plain Name', { format: 'VST3', category: 'Delay|Fx' }))).toEqual(['utility']);
  });

  it('a declared category and name keywords add up, listed in display order', () => {
    // Guitar Rig as a VST3 declares Distortion: amp sim and drive.
    expect(categorize(fx('Guitar Rig 7', { format: 'VST3', category: 'Fx|Distortion' }))).toEqual(['drive', 'amps-cabs']);
    expect(categorize(fx('NeuralAmpModeler', { format: 'VST3', category: 'Fx|Distortion' }))).toEqual(['drive', 'amps-cabs']);
    expect(categorize(fx('Tube Delay'))).toEqual(['drive', 'delay']);
    expect(categorize(fx('Shimmer Pitch Verb'))).toEqual(['reverb', 'pitch']);
    expect(categorize(fx('Spring Reverb', { format: 'VST3', category: 'Fx|Reverb' }))).toEqual(['reverb']); // no duplicates
  });
});

describe('categorize: keyword matching', () => {
  it('matches keywords at the start of words, so other words containing them do not count', () => {
    expect(categorize(fx('Rolling Sampler'))).toEqual(['utility']); // "Sampler" is not "amp"
    expect(categorize(fx('Trigger'))).toEqual(['utility']); // not "rig"
    expect(categorize(fx('Parameter Box'))).toEqual(['utility']); // not "meter"
    expect(categorize(fx('Pirate Ratio'))).toEqual(['utility']); // "rat" is a whole word only
  });

  it('whole-word-only keywords', () => {
    expect(categorize(fx('Klon OD'))).toEqual(['drive']);
    expect(categorize(fx('Rat'))).toEqual(['drive']);
    expect(categorize(fx('NAM Loader'))).toEqual(['amps-cabs']);
    expect(categorize(fx('Odin'))).toEqual(['utility']);
  });

  it('multi-word keywords', () => {
    expect(categorize(fx('Ring Mod'))).toEqual(['modulation']);
    expect(categorize(fx('IR Loader'))).toEqual(['amps-cabs']);
    expect(categorize(fx('Tape Stop'))).toEqual(['delay']);
    expect(categorize(fx('Uni-V'))).toEqual(['modulation']);
  });

  it('more guitar names', () => {
    expect(categorize(fx('Tube Screamer'))).toEqual(['drive']);
    expect(categorize(fx('Big Muff Fuzz'))).toEqual(['drive']);
    expect(categorize(fx('Helix Native'))).toEqual(['amps-cabs']);
    expect(categorize(fx('Archetype: Plini X'))).toEqual(['amps-cabs']);
    expect(categorize(fx('TONEX'))).toEqual(['amps-cabs']);
    expect(categorize(fx('Cab Sim'))).toEqual(['amps-cabs']);
    expect(categorize(fx('Rotary Leslie'))).toEqual(['modulation']);
    expect(categorize(fx('Crybaby Wah'))).toEqual(['filter-wah']);
    expect(categorize(fx('Whammy'))).toEqual(['pitch']);
    expect(categorize(fx('Octaver'))).toEqual(['pitch']);
  });
});

describe('GROUPS', () => {
  it('lists the groups in display order with their labels and whether they are guitar groups', () => {
    expect(GROUPS.map((g) => [g.id, g.label, g.guitar])).toEqual([
      ['drive', 'Drive', true],
      ['amps-cabs', 'Amps & Cabs', true],
      ['modulation', 'Modulation', true],
      ['delay', 'Delay', true],
      ['reverb', 'Reverb', true],
      ['dynamics', 'Dynamics', true],
      ['eq', 'EQ', true],
      ['filter-wah', 'Filter & Wah', true],
      ['pitch', 'Pitch', true],
      ['utility', 'Utility & Other', false],
      ['instrument', 'Instruments', false],
    ]);
  });
});

describe('filterPlugins', () => {
  const list = [
    fx('kHs Delay', { vendor: 'Kilohearts' }),
    fx('kHs Gain', { vendor: 'Kilohearts' }),
    fx('MDelay', { vendor: 'MeldaProduction' }),
    fx('Raum', { vendor: 'Native Instruments' }),
    synth('Massive', { vendor: 'Native Instruments' }),
    fx('SPAN', { vendor: 'Voxengo' }),
  ];
  const names = (l: PluginInfo[]) => l.map((p) => p.name);

  it('returns every plugin, in order, for an empty query with the guitar filter off', () => {
    expect(names(filterPlugins(list, '', false))).toEqual(['kHs Delay', 'kHs Gain', 'MDelay', 'Raum', 'Massive', 'SPAN']);
    expect(names(filterPlugins(list, '   ', false))).toHaveLength(6);
  });

  it('matches the name or the vendor, case-insensitively, every word somewhere', () => {
    expect(names(filterPlugins(list, 'delay', false))).toEqual(['kHs Delay', 'MDelay']);
    expect(names(filterPlugins(list, 'KILO', false))).toEqual(['kHs Delay', 'kHs Gain']);
    expect(names(filterPlugins(list, 'native', false))).toEqual(['Raum', 'Massive']);
    expect(names(filterPlugins(list, 'kilohearts delay', false))).toEqual(['kHs Delay']);
    expect(names(filterPlugins(list, 'melda  delay', false))).toEqual(['MDelay']);
    expect(names(filterPlugins(list, 'voxengo delay', false))).toEqual([]);
  });

  it('the guitar filter hides plugins that are only utility or instrument', () => {
    expect(names(filterPlugins(list, '', true))).toEqual(['kHs Delay', 'MDelay', 'Raum']);
    expect(names(filterPlugins(list, 'native', true))).toEqual(['Raum']);
    expect(names(filterPlugins(list, 'span', true))).toEqual([]);
  });

  it('keeps a plugin that is utility and also in a guitar group', () => {
    const both = fx('Stereo Chorus'); // utility ("stereo") + modulation
    expect(categorize(both)).toEqual(['modulation', 'utility']);
    expect(filterPlugins([both], '', true)).toEqual([both]);
  });
});

describe('groupPlugins', () => {
  it('puts each plugin in every one of its groups, groups in display order, names sorted, empty groups left out', () => {
    const rig = fx('Guitar Rig 7', { format: 'VST3', category: 'Fx|Distortion' });
    const groups = groupPlugins([fx('Raum'), synth('Massive'), rig, fx('Dirt'), fx('Bite'), fx('kHs Gain')]);
    expect(groups.map((g) => [g.group.id, g.plugins.map((p) => p.name)])).toEqual([
      ['drive', ['Bite', 'Dirt', 'Guitar Rig 7']],
      ['amps-cabs', ['Guitar Rig 7']],
      ['reverb', ['Raum']],
      ['utility', ['kHs Gain']],
      ['instrument', ['Massive']],
    ]);
  });

  it('orders the same name by format', () => {
    const groups = groupPlugins([fx('Raum', { format: 'VST3' }), fx('Raum', { format: 'AudioUnit' })]);
    expect(groups[0].plugins.map((p) => p.format)).toEqual(['AudioUnit', 'VST3']);
  });
});
