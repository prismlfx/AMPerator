// Node art (spec §6): the picture shown for a node, first available of
//   1. the user's image (node.art.image, a file in the library's Art/ folder),
//   2. the asset's provider image (node.art.providerImage; unused until sub-project 3),
//   3. a snapshot of the plugin's editor (node.art.snapshot, Art/cache/; Plugin nodes only),
//   4. built-in archetype art: node.art.archetype, else guessed from the node's type and name, else generic.
// The built-in drawings (./*.svg) are generic gear with no brand names or logos. The keyword lists below only
// match user / model / plugin names; they are never shown.

import type { RigNode } from '../state';

export interface NodeArt {
  archetype?: string;
  image?: string; // absolute path of the user's image in Art/
  providerImage?: string; // URL (sub-project 3)
  snapshot?: string; // absolute path of the plugin editor snapshot in Art/cache/
}

export type NodeView = RigNode & { art?: NodeArt };

export const ARCHETYPES = {
  amp: ['british-stack', 'american-clean-combo', 'high-gain-rectifier', 'ac-chime', 'amp-generic'],
  cab: ['cab-4x12', 'cab-2x12', 'cab-1x12-open'],
  pedal: ['drive', 'fuzz', 'delay', 'chorus', 'reverb', 'eq', 'gate', 'pedal-generic'],
} as const;

export type ArtKind = keyof typeof ARCHETYPES;
export const ALL_ARCHETYPES: readonly string[] = [...ARCHETYPES.amp, ...ARCHETYPES.cab, ...ARCHETYPES.pedal];

/** Human label for an archetype id ("high-gain-rectifier" -> "High gain rectifier", "cab-4x12" -> "4x12"). */
export function archetypeLabel(id: string): string {
  const s = id.replace(/^cab-/, '').replace(/-/g, ' ');
  return s.charAt(0).toUpperCase() + s.slice(1);
}

const SVGS: Record<string, string> = Object.fromEntries(
  Object.entries(import.meta.glob<string>('./*.svg', { query: '?raw', import: 'default', eager: true })).map(([path, svg]) => [
    path.replace(/^\.\//, '').replace(/\.svg$/, ''),
    svg,
  ]),
);

export function archetypeSvg(id: string): string {
  return SVGS[id] ?? '';
}

const urlCache = new Map<string, string>();
/** data: URL of a built-in drawing (for <img src>). */
export function archetypeUrl(id: string): string {
  let url = urlCache.get(id);
  if (url === undefined) {
    url = `data:image/svg+xml;charset=utf-8,${encodeURIComponent(archetypeSvg(id))}`;
    urlCache.set(id, url);
  }
  return url;
}

export function artKind(type: string): ArtKind {
  return type === 'NamAmp' ? 'amp' : type === 'CabIR' ? 'cab' : 'pedal';
}

// Keywords (case-insensitive), first match wins. Short ones are whole words so "od" doesn't match "mode".
const AMP_RULES: [RegExp, string][] = [
  [/plexi|jcm|marshall|british|\bbrit/i, 'british-stack'],
  [/\brect|mesa|\bdual\b|5150|\bevh\b|high ?gain|djent/i, 'high-gain-rectifier'],
  [/vox|ac30|ac15|chime/i, 'ac-chime'],
  [/fender|\btwin\b|deluxe|blackface|bassman|clean/i, 'american-clean-combo'],
];
const CAB_RULES: [RegExp, string][] = [
  [/4x12|\b412\b/i, 'cab-4x12'],
  [/2x12|\b212\b/i, 'cab-2x12'],
  [/1x12|\b112\b|\bopen\b/i, 'cab-1x12-open'],
];
const PEDAL_RULES: [RegExp, string][] = [
  [/drive|\bod\b|tube ?screamer|\bts9\b|ts808|klon|blues/i, 'drive'],
  [/fuzz|muff|\bface\b/i, 'fuzz'],
  [/delay|echo/i, 'delay'],
  [/chorus|flanger|phaser|vibe|\bmod\b|modulat/i, 'chorus'],
  [/reverb|verb\b|\bhall\b|\bplate\b|spring|shimmer/i, 'reverb'],
  [/\beq\b|equali[sz]er/i, 'eq'],
  [/gate/i, 'gate'],
];

const GENERIC: Record<ArtKind, string> = { amp: 'amp-generic', cab: 'cab-2x12', pedal: 'pedal-generic' };

function match(rules: [RegExp, string][], name: string): string | undefined {
  return rules.find(([re]) => re.test(name))?.[1];
}

/** Built-in art for a node of `type` named `name` (model / IR file name, plugin name). */
export function guessArchetype(type: string, name: string): string {
  switch (type) {
    case 'Gate':
      return 'gate';
    case 'EQ':
      return 'eq';
    case 'NamAmp':
      return match(AMP_RULES, name) ?? GENERIC.amp;
    case 'CabIR':
      return match(CAB_RULES, name) ?? GENERIC.cab;
    case 'Plugin':
      // A plugin may be a pedal, an amp sim or a cab loader.
      return match(PEDAL_RULES, name) ?? match(AMP_RULES, name) ?? match(CAB_RULES, name) ?? GENERIC.pedal;
    default:
      return GENERIC.pedal;
  }
}

/** URL the web view loads a file in the library's Art/ folder from (the app serves "art/<path>", see
    docs/bridge-protocol.md); "" for paths outside Art/. In the Vite dev server the page is not served by the
    app, so the app's resource-provider origin is used explicitly. */
export function artUrl(absolutePath: string | undefined): string {
  if (!absolutePath) return '';
  const p = absolutePath.replace(/\\/g, '/');
  const i = p.lastIndexOf('/Art/');
  if (i < 0) return '';
  const rel = p.slice(i + '/Art/'.length);
  if (!rel || rel.split('/').some((part) => part === '' || part === '.' || part === '..')) return '';
  const base = typeof location !== 'undefined' && location.port === '5173' ? 'juce://juce.backend/' : '';
  return base + 'art/' + rel.split('/').map(encodeURIComponent).join('/');
}

/** The name art keywords are matched against: plugin name or the asset's file name. */
export function nodeArtName(n: RigNode): string {
  const plugin = n.plugin as { name?: string } | undefined;
  const file = (n.asset as { file?: string } | undefined)?.file ?? '';
  return plugin?.name || file.split(/[\\/]/).pop()!.replace(/\.[^.]+$/, '');
}

/** The archetype a node is drawn with when it has no image: its own (if known), else a guess. */
export function nodeArchetype(n: NodeView): string {
  const own = n.art?.archetype;
  if (own && SVGS[own]) return own;
  return guessArchetype(n.type, nodeArtName(n));
}

/** The picture for node `n` (an <img src>), following the order above. */
export function resolveArt(n: NodeView): string {
  const art = n.art ?? {};
  const image = artUrl(art.image);
  if (image) return image;
  if (art.providerImage) return art.providerImage;
  if (n.type === 'Plugin') {
    const snapshot = artUrl(art.snapshot);
    if (snapshot) return snapshot;
  }
  return archetypeUrl(nodeArchetype(n));
}
