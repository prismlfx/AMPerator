import { describe, expect, it } from 'vitest';
import { ARCHETYPES, ALL_ARCHETYPES, archetypeSvg, archetypeUrl, artUrl, guessArchetype, resolveArt, type NodeView } from './archetypes';

const node = (type: string, art: NodeView['art'] = {}, extra: Partial<NodeView> = {}): NodeView => ({
  id: 'n', type, bypass: false, params: {}, art, ...extra,
});

describe('resolveArt', () => {
  it('user image wins over everything', () => {
    const n = node('Plugin', {
      image: '/Users/me/Music/AMPerator/Art/my photo.png',
      providerImage: 'https://example.invalid/amp.jpg',
      snapshot: '/Users/me/Music/AMPerator/Art/cache/VST3-Delay.png',
      archetype: 'delay',
    });
    expect(resolveArt(n)).toBe('art/my%20photo.png');
  });

  it('provider image comes before the plugin snapshot', () => {
    const n = node('Plugin', { providerImage: 'https://example.invalid/amp.jpg', snapshot: '/lib/Art/cache/x.png' });
    expect(resolveArt(n)).toBe('https://example.invalid/amp.jpg');
  });

  it('plugin snapshot used for Plugin nodes without user image', () => {
    const n = node('Plugin', { image: '', snapshot: '/lib/Art/cache/VST3-Delay.png', archetype: 'delay' });
    expect(resolveArt(n)).toBe('art/cache/VST3-Delay.png');
  });

  it('snapshots only count for Plugin nodes', () => {
    const n = node('NamAmp', { snapshot: '/lib/Art/cache/x.png', archetype: 'ac-chime' });
    expect(resolveArt(n)).toBe(archetypeUrl('ac-chime'));
  });

  it('uses the node archetype, else a guess from type and name', () => {
    expect(resolveArt(node('NamAmp', { archetype: 'british-stack' }))).toBe(archetypeUrl('british-stack'));
    expect(resolveArt(node('NamAmp', {}, { asset: { file: '/m/Big Rectifier ch2.nam' } }))).toBe(archetypeUrl('high-gain-rectifier'));
    expect(resolveArt(node('Plugin', {}, { plugin: { name: 'Valhalla Delay' } }))).toBe(archetypeUrl('delay'));
    expect(resolveArt(node('Gate'))).toBe(archetypeUrl('gate'));
    expect(resolveArt(node('Gate', { archetype: 'no-such-art' }))).toBe(archetypeUrl('gate')); // unknown ids are ignored
  });

  it('nodes without art fall back to an archetype', () => {
    expect(resolveArt({ id: 'x', type: 'CabIR', bypass: false, params: {} })).toBe(archetypeUrl('cab-2x12'));
  });

  it('a user image outside Art/ is not turned into a URL', () => {
    expect(artUrl('/etc/passwd')).toBe('');
    expect(artUrl('C:\\Users\\me\\Documents\\AMPerator\\Art\\a b.png')).toBe('art/a%20b.png');
    expect(resolveArt(node('Gate', { image: '/tmp/x.png' }))).toBe(archetypeUrl('gate'));
  });
});

describe('guessArchetype', () => {
  it('matches pedal keywords for plugins', () => {
    expect(guessArchetype('Plugin', 'Valhalla Delay')).toBe('delay');
    expect(guessArchetype('Plugin', 'Big Muff Pi')).toBe('fuzz');
    expect(guessArchetype('Plugin', 'TS808 Overdrive')).toBe('drive');
    expect(guessArchetype('Plugin', 'Vintage OD')).toBe('drive');
    expect(guessArchetype('Plugin', 'Tape Echo')).toBe('delay');
    expect(guessArchetype('Plugin', 'Stereo Phaser')).toBe('chorus');
    expect(guessArchetype('Plugin', 'Shimmer Hall')).toBe('reverb');
    expect(guessArchetype('Plugin', 'Pro EQ 3')).toBe('eq');
    expect(guessArchetype('Plugin', 'Noise Gate')).toBe('gate');
  });

  it('matches amp keywords for amps (case-insensitive)', () => {
    expect(guessArchetype('NamAmp', 'Rectifier ch2')).toBe('high-gain-rectifier');
    expect(guessArchetype('NamAmp', 'JCM800 crunch')).toBe('british-stack');
    expect(guessArchetype('NamAmp', 'PLEXI lead')).toBe('british-stack');
    expect(guessArchetype('NamAmp', 'Deluxe Reverb clean')).toBe('american-clean-combo');
    expect(guessArchetype('NamAmp', 'AC30 top boost')).toBe('ac-chime');
    expect(guessArchetype('NamAmp', '5150 djent')).toBe('high-gain-rectifier');
  });

  it('matches cab keywords for cabs', () => {
    expect(guessArchetype('CabIR', 'V30 4x12 SM57')).toBe('cab-4x12');
    expect(guessArchetype('CabIR', 'Blue 212')).toBe('cab-2x12');
    expect(guessArchetype('CabIR', 'Open back 1x12')).toBe('cab-1x12-open');
  });

  it('does not match keywords inside other words', () => {
    expect(guessArchetype('Plugin', 'Model Mode')).toBe('pedal-generic'); // not "od" / "mod"
    expect(guessArchetype('Plugin', 'Sequencer')).toBe('pedal-generic'); // not "eq"
  });

  it('unknown falls back to generic per type', () => {
    expect(guessArchetype('NamAmp', 'My Capture 7')).toBe('amp-generic');
    expect(guessArchetype('CabIR', 'IR 7')).toBe('cab-2x12');
    expect(guessArchetype('Plugin', 'Mystery Box')).toBe('pedal-generic');
    expect(guessArchetype('Gate', '')).toBe('gate');
    expect(guessArchetype('EQ', '')).toBe('eq');
    expect(guessArchetype('Placeholder', '')).toBe('pedal-generic');
  });

  it('a plugin named like an amp or a cab gets that art', () => {
    expect(guessArchetype('Plugin', 'Plexi Sim')).toBe('british-stack');
    expect(guessArchetype('Plugin', 'Cab 4x12')).toBe('cab-4x12');
  });
});

describe('built-in art', () => {
  it('has an SVG for every archetype, grouped per kind', () => {
    expect(ARCHETYPES.amp).toEqual(['british-stack', 'american-clean-combo', 'high-gain-rectifier', 'ac-chime', 'amp-generic']);
    expect(ARCHETYPES.cab).toEqual(['cab-4x12', 'cab-2x12', 'cab-1x12-open']);
    expect(ARCHETYPES.pedal).toEqual(['drive', 'fuzz', 'delay', 'chorus', 'reverb', 'eq', 'gate', 'pedal-generic']);
    expect(ALL_ARCHETYPES).toHaveLength(16);
    for (const id of ALL_ARCHETYPES) {
      expect(archetypeSvg(id), id).toMatch(/^<svg[\s\S]*<\/svg>\s*$/);
      expect(archetypeUrl(id)).toMatch(/^data:image\/svg\+xml/);
    }
    expect(new Set(ALL_ARCHETYPES.map(archetypeSvg)).size).toBe(16); // all different drawings
  });

  it('contains no text, scripts, external references or brand names', () => {
    const brands = /marshall|fender|mesa|boogie|vox|orange|peavey|evh|ibanez|boss|electro|mxr|strymon|eventide|klon|dunlop|celestion|jensen|line ?6|tube ?screamer|big ?muff/i;
    for (const id of ALL_ARCHETYPES) {
      const svg = archetypeSvg(id);
      expect(svg, id).not.toMatch(/<text|<script|<image|href\s*=|url\((?!#)/i);
      expect(svg, id).not.toMatch(brands);
    }
  });
});
