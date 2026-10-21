// Library search: which rigs a text query and a set of selected tags let through (pure; used by the library panel).

export interface Filterable {
  name: string;
  band: string;
  tags?: string[]; // missing in replies of engines without tags
}

/** The query's words, lower-cased ("  Wild EYES " -> ["wild", "eyes"]). */
export function queryWords(query: string): string[] {
  return query.toLowerCase().split(/\s+/).filter((w) => w.length > 0);
}

/** Whether the rig carries `tag` (a whole tag, case-insensitive). */
export function hasTag(rig: Filterable, tag: string): boolean {
  const t = tag.trim().toLowerCase();
  return (rig.tags ?? []).some((x) => x.trim().toLowerCase() === t);
}

/** The rigs, in their order, that match every word of `query` somewhere (case-insensitive substring of the name,
    the band or any tag) and carry every tag in `selectedTags`. */
export function filterRigs<T extends Filterable>(entries: readonly T[], query: string, selectedTags: readonly string[]): T[] {
  const words = queryWords(query);
  return entries.filter((r) => {
    if (!selectedTags.every((t) => hasTag(r, t))) return false;
    if (words.length === 0) return true;
    const fields = [r.name, r.band, ...(r.tags ?? [])].map((f) => (f ?? '').toLowerCase());
    return words.every((w) => fields.some((f) => f.includes(w)));
  });
}
