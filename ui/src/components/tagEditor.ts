// Tag editor: the rig's tags as chips (✕ removes one) and an input that adds one on Enter or comma; Backspace on
// the empty input removes the last chip, Escape leaves the field. Known tags whose start matches the typed text are
// offered in a dropdown (↑ / ↓ to pick, Enter or click to add). Every change reports the complete list.

import { el } from '../dialog';

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

  const label = opts.label ?? 'Tags';
  const input = el('input', {
    class: 'tag-input',
    type: 'text',
    placeholder: 'Add tag…',
    'aria-label': `Add tag`,
    maxlength: String(MAX_TAG_LENGTH),
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

  /** Adds each non-blank tag that is not there yet; one report for all of them. */
  function addTags(candidates: string[]) {
    const next = [...tags];
    for (const raw of candidates) {
      const t = raw.trim();
      if (t && !next.some((x) => same(x, t))) next.push(t);
    }
    if (next.length !== tags.length) commit(next);
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
  input.addEventListener('blur', closeList);

  input.addEventListener('input', () => {
    if (input.value.includes(',')) {
      const parts = input.value.split(',');
      input.value = parts.pop() ?? '';
      addTags(parts);
    }
    activeIndex = -1;
    renderList();
  });

  input.addEventListener('keydown', (e) => {
    switch (e.key) {
      case 'Enter':
      case ',': {
        e.preventDefault();
        if (e.key === 'Enter' && activeIndex >= 0 && shown[activeIndex]) return pick(shown[activeIndex]);
        const text = input.value;
        input.value = '';
        closeList();
        addTags([text]);
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
      fetchId++;
      root.remove();
    },
  };
}
