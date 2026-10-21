import { describe, expect, it } from 'vitest';
import { filterRigs, hasTag, queryWords } from './rigFilter';

const rigs = [
  { id: 'djent', name: 'Peavey 5150 - Djent', band: 'Metal', tags: ['Parkway Drive', 'Wild Eyes', 'metal'] },
  { id: 'plush', name: 'Plush', band: 'STP', tags: ['clean', '90s'] },
  { id: 'dog', name: 'Black Dog', band: 'Zeppelin', tags: ['70s', 'crunch'] },
  { id: 'kash', name: 'Kashmir', band: 'Zeppelin', tags: ['70s', 'clean'] },
  { id: 'none', name: 'Untagged', band: 'My Tones' }, // older listRigs replies carry no tags
];
const ids = (list: { id: string }[]) => list.map((r) => r.id);

describe('filterRigs', () => {
  it('returns every rig, in order, for an empty query and no selected tags', () => {
    expect(ids(filterRigs(rigs, '', []))).toEqual(['djent', 'plush', 'dog', 'kash', 'none']);
    expect(ids(filterRigs(rigs, '   ', []))).toEqual(['djent', 'plush', 'dog', 'kash', 'none']);
  });

  it('matches the query against the rig name, the band and the tags', () => {
    expect(ids(filterRigs(rigs, 'kashmir', []))).toEqual(['kash']); // name
    expect(ids(filterRigs(rigs, 'zeppelin', []))).toEqual(['dog', 'kash']); // band
    expect(ids(filterRigs(rigs, 'Parkway Drive', []))).toEqual(['djent']); // a tag (song / band names)
    expect(ids(filterRigs(rigs, 'wild eyes', []))).toEqual(['djent']);
    expect(ids(filterRigs(rigs, 'clean', []))).toEqual(['plush', 'kash']);
  });

  it('is a case-insensitive substring match', () => {
    expect(ids(filterRigs(rigs, 'PARK', []))).toEqual(['djent']);
    expect(ids(filterRigs(rigs, 'dog', []))).toEqual(['dog']);
    expect(ids(filterRigs(rigs, '515', []))).toEqual(['djent']);
    expect(ids(filterRigs(rigs, 'zzz', []))).toEqual([]);
  });

  it('needs every word of the query to match somewhere (name, band or a tag)', () => {
    expect(ids(filterRigs(rigs, 'zeppelin clean', []))).toEqual(['kash']); // band + tag
    expect(ids(filterRigs(rigs, '70s  dog', []))).toEqual(['dog']); // tag + name, extra spaces ignored
    expect(ids(filterRigs(rigs, 'eyes peavey metal', []))).toEqual(['djent']);
    expect(ids(filterRigs(rigs, 'zeppelin metal', []))).toEqual([]);
  });

  it('keeps only rigs that carry every selected tag, case-insensitively', () => {
    expect(ids(filterRigs(rigs, '', ['70s']))).toEqual(['dog', 'kash']);
    expect(ids(filterRigs(rigs, '', ['70s', 'Clean']))).toEqual(['kash']);
    expect(ids(filterRigs(rigs, '', ['parkway drive']))).toEqual(['djent']);
    expect(ids(filterRigs(rigs, '', ['clean', 'metal']))).toEqual([]);
  });

  it('a selected tag must match a whole tag, not part of one', () => {
    expect(ids(filterRigs(rigs, '', ['Parkway']))).toEqual([]);
    expect(ids(filterRigs(rigs, '', ['0s']))).toEqual([]);
  });

  it('combines the query and the selected tags', () => {
    expect(ids(filterRigs(rigs, 'dog', ['70s']))).toEqual(['dog']);
    expect(ids(filterRigs(rigs, 'plush', ['70s']))).toEqual([]);
  });
});

describe('helpers', () => {
  it('queryWords splits on whitespace and lower-cases', () => {
    expect(queryWords('  Wild   EYES ')).toEqual(['wild', 'eyes']);
    expect(queryWords('')).toEqual([]);
  });

  it('hasTag compares whole tags case-insensitively', () => {
    expect(hasTag(rigs[0], 'WILD EYES')).toBe(true);
    expect(hasTag(rigs[0], 'wild')).toBe(false);
    expect(hasTag(rigs[4], 'clean')).toBe(false);
  });
});
