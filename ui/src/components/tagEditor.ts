// Tag editor: the rig's tags as chips (✕ removes one) and an input that adds one on Enter, comma or leaving the
// field; Backspace on the empty input removes the last chip, Escape discards the typed text and leaves the field.
// Known tags whose start matches the typed text are offered in a dropdown (↑ / ↓ to pick, Enter or click to add).
// A tag longer than MAX_TAG_LENGTH stays in the input with a toast saying why. Every change reports the complete list.

import { el, toast } from '../dialog';

export const MAX_TAG_LENGTH = 40; // Engine::maxTagLength
const MAX_SUGGESTIONS = 8;

export interface TagEditorOptions {
  tags: string[];
  /** Called with the complete new list after every add / remove. */
  onChange: (tags: string[]) => void;
  /** Every known tag (e.g. listTags), best first; asked for each time the input gets focus. */
  suggestions?: () => Promise<string[]> | string[];
  label?: string;
}

export interface TagEditor {
  el: HTMLElement;
  input: HTMLInputElement;
  /** Shows `tags` (e.g. the engine's confirmed list) without reporting a change; typed text is kept. */
  setTags(tags: string[]): void;
  dispose(): void;
}

const same = (a: string, b: string) => a.trim().toLowerCase() === b.trim().toLowerCase();

export function createTagEditor(opts: TagEditorOptions): TagEditor {
  let tags = [...opts.tags];
  let known: string[] = [];
  let shown: string[] = []; // suggestions in the dropdown
  let activeIndex = -1;
  let fetchId = 0;
  let disposed = false;
  let lastRefused = ''; // the too-long text last toasted (no repeat toast while it is being edited)

  const label = opts.label ?? 'Tags';
  const input = el('input', {
    class: 'tag-input',
    type: 'text',
    placeholder: 'Add tag…',
    'aria-label': `Add tag`, // no maxlength: it would cut pasted comma lists; addTags checks each tag
    spellcheck: 'false',
    autocomplete: 'off',
    role: 'combobox',
    'aria-autocomplete': 'list',
    'aria-expanded': 'false',
  });
  const list = el('ul', { class: 'tag-suggestions', role: 'listbox', 'aria-label': 'Known tags' });
  list.hidden = true;
  const root = el('div', { class: 'tag-editor', role: 'group', 'aria-label': label }, input, list);

  function renderChips() {
    for (const c of [...root.querySelectorAll('.tag-chip')]) c.remove();
    input.before(
      ...tags.map((t) => {
        const remove = el('button', { class: 'tag-remove', type: 'button', 'aria-label': `Remove tag ${t}`, title: 'Remove' }, '✕');
        remove.addEventListener('click', () => removeTag(t));
        return el('span', { class: 'tag-chip' }, el('span', { class: 'tag-text' }, t), remove);
      }),
    );
  }

  function commit(next: string[]) {
    tags = next;
    renderChips();
    opts.onChange([...tags]);
  }

  /** Adds each non-blank tag that is not there yet (one report for all of them) and returns the ones refused for
      being longer than MAX_TAG_LENGTH, after a toast unless `quietIfSame` and the same text was toasted last. */
  function addTags(candidates: string[], quietIfSame = false): string[] {
    const next = [...tags];
    const refused: string[] = [];
    for (const raw of candidates) {
      const t = raw.trim();
      if (t.length > MAX_TAG_LENGTH) refused.push(t);
      else if (t && !next.some((x) => same(x, t))) next.push(t);
    }
    if (next.length !== tags.length) commit(next);
    const refusedKey = refused.join('\n');
    if (refused.length > 0 && !(quietIfSame && refusedKey === lastRefused)) {
      const shown = refused[0].length > 24 ? `${refused[0].slice(0, 24)}…` : refused[0];
      toast(`A tag can have at most ${MAX_TAG_LENGTH} characters: “${shown}” is ${refused[0].length}.`);
    }
    lastRefused = refusedKey;
    return refused;
  }

  /** Adds the input's text as a tag; a refused (too long) text stays in the input. */
  function commitInput() {
    const text = input.value;
    input.value = '';
    closeList();
    const refused = addTags([text]);
    if (refused.length > 0) input.value = text.trim();
  }

  function removeTag(t: string) {
    commit(tags.filter((x) => x !== t));
  }

  function closeList() {
    shown = [];
    activeIndex = -1;
    list.replaceChildren();
    list.hidden = true;
    input.setAttribute('aria-expanded', 'false');
  }

  function renderList() {
    const prefix = input.value.trim().toLowerCase();
    shown = prefix
      ? known.filter((k) => k.toLowerCase().startsWith(prefix) && !tags.some((t) => same(t, k))).slice(0, MAX_SUGGESTIONS)
      : [];
    if (shown.length === 0) return closeList();
    activeIndex = Math.min(activeIndex, shown.length - 1);
    list.replaceChildren(
      ...shown.map((s, i) => {
        const item = el('li', { class: 'tag-suggestion', role: 'option', 'aria-selected': String(i === activeIndex) }, s);
        item.addEventListener('mousedown', (e) => {
          e.preventDefault(); // keep the focus in the input
          pick(s);
        });
        return item;
      }),
    );
    list.hidden = false;
    // Under the text being typed (the input wraps onto its own line after many chips).
    list.style.left = `${input.offsetLeft}px`;
    list.style.top = `${input.offsetTop + input.offsetHeight}px`;
    input.setAttribute('aria-expanded', 'true');
  }

  function pick(s: string) {
    input.value = '';
    closeList();
    addTags([s]);
  }

  input.addEventListener('focus', async () => {
    if (!opts.suggestions) return;
    const id = ++fetchId;
    try {
      const k = await opts.suggestions();
      if (id === fetchId) {
        known = k;
        if (document.activeElement === input) renderList();
      }
    } catch {
      // suggestions are a convenience: typing still works without them
    }
  });
  input.addEventListener('blur', () => {
    closeList();
    if (!disposed && input.value.trim()) commitInput(); // typed text is not lost by clicking elsewhere
  });

  input.addEventListener('input', () => {
    if (input.value.includes(',')) {
      const parts = input.value.split(',');
      const rest = parts.pop() ?? '';
      const refused = addTags(parts, true);
      // Refused parts stay in the input to be fixed, ahead of the part still being typed.
      input.value = refused.length > 0 ? [...refused, ...(rest.trim() ? [rest.trim()] : [])].join(', ') : rest;
    }
    activeIndex = -1;
    renderList();
  });

  input.addEventListener('keydown', (e) => {
    switch (e.key) {
      case 'Enter':
      case ',': {
        if (e.isComposing) return; // Enter confirms an IME composition, not the tag
        e.preventDefault();
        if (e.key === 'Enter' && activeIndex >= 0 && shown[activeIndex]) return pick(shown[activeIndex]);
        commitInput();
        return;
      }
      case 'Backspace':
        if (input.value === '' && tags.length > 0) {
          e.preventDefault();
          commit(tags.slice(0, -1));
        }
        return;
      case 'ArrowDown':
      case 'ArrowUp':
        if (shown.length === 0) return;
        e.preventDefault();
        activeIndex =
          e.key === 'ArrowDown' ? Math.min(shown.length - 1, activeIndex + 1) : Math.max(0, activeIndex - 1);
        renderList();
        return;
      case 'Escape':
        e.preventDefault();
        e.stopPropagation(); // leaving the field is all Escape does here
        input.value = ''; // discarded, so leaving the field adds nothing
        closeList();
        input.blur();
        return;
    }
  });

  renderChips();

  return {
    el: root,
    input,
    setTags(next: string[]) {
      tags = [...next];
      renderChips();
      if (!list.hidden) renderList();
    },
    dispose() {
      disposed = true;
      fetchId++;
      root.remove();
    },
  };
}
