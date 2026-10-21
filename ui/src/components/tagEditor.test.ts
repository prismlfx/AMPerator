import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { createTagEditor, type TagEditor } from './tagEditor';

let editor: TagEditor;
let onChange: ReturnType<typeof vi.fn<(tags: string[]) => void>>;
const KNOWN = ['clean', 'Parkway Drive', 'Pantera', '70s', 'metal'];

const flush = () => new Promise((r) => setTimeout(r, 0));
const chips = () => [...editor.el.querySelectorAll('.tag-chip .tag-text')].map((c) => c.textContent);
const suggestions = () => [...document.querySelectorAll('.tag-suggestion')].map((s) => s.textContent);
const active = () => document.querySelector('.tag-suggestion[aria-selected="true"]')?.textContent;
const key = (k: string) =>
  editor.input.dispatchEvent(new KeyboardEvent('keydown', { key: k, bubbles: true, cancelable: true }));
function type(text: string) {
  editor.input.value = text;
  editor.input.dispatchEvent(new Event('input', { bubbles: true }));
}

function mount(tags: string[] = ['clean']) {
  onChange = vi.fn<(tags: string[]) => void>();
  editor = createTagEditor({ tags, onChange, suggestions: async () => KNOWN });
  document.body.append(editor.el);
}

beforeEach(() => {
  document.body.innerHTML = '';
});
afterEach(() => editor?.dispose());

describe('tag editor', () => {
  it('shows the tags as chips', () => {
    mount(['clean', 'Parkway Drive']);
    expect(chips()).toEqual(['clean', 'Parkway Drive']);
    expect(editor.input.placeholder).toContain('tag');
  });

  it('Enter adds the typed tag (trimmed) and reports the full list', () => {
    mount();
    type('  Wild Eyes ');
    key('Enter');
    expect(onChange).toHaveBeenLastCalledWith(['clean', 'Wild Eyes']);
    expect(chips()).toEqual(['clean', 'Wild Eyes']);
    expect(editor.input.value).toBe('');
  });

  it('a comma adds the tag too, also for pasted comma-separated text', () => {
    mount([]);
    type('metal');
    key(',');
    expect(onChange).toHaveBeenLastCalledWith(['metal']);
    type('Parkway Drive, Wild Eyes,djent');
    expect(onChange).toHaveBeenLastCalledWith(['metal', 'Parkway Drive', 'Wild Eyes']);
    expect(editor.input.value).toBe('djent'); // the part after the last comma is still being typed
  });

  it('ignores blank input and tags already present (case-insensitive)', () => {
    mount(['clean']);
    type('   ');
    key('Enter');
    type('CLEAN');
    key('Enter');
    expect(onChange).not.toHaveBeenCalled();
    expect(chips()).toEqual(['clean']);
    expect(editor.input.value).toBe('');
  });

  it('✕ removes a tag', () => {
    mount(['clean', '70s', 'metal']);
    editor.el.querySelector<HTMLButtonElement>('.tag-chip:nth-child(2) .tag-remove')!.click();
    expect(onChange).toHaveBeenLastCalledWith(['clean', 'metal']);
    expect(chips()).toEqual(['clean', 'metal']);
    expect(editor.el.querySelector('.tag-remove')!.getAttribute('aria-label')).toBe('Remove tag clean');
  });

  it('Backspace on an empty input removes the last tag; with text it only edits the text', () => {
    mount(['clean', '70s']);
    type('ab');
    key('Backspace');
    expect(onChange).not.toHaveBeenCalled();
    type('');
    key('Backspace');
    expect(onChange).toHaveBeenLastCalledWith(['clean']);
    key('Backspace');
    expect(onChange).toHaveBeenLastCalledWith([]);
    key('Backspace'); // nothing left
    expect(onChange).toHaveBeenCalledTimes(2);
  });

  it('Escape blurs the input', () => {
    mount();
    editor.input.focus();
    expect(document.activeElement).toBe(editor.input);
    key('Escape');
    expect(document.activeElement).not.toBe(editor.input);
  });

  it('suggests known tags matching the typed prefix, without tags already present', async () => {
    mount(['clean']);
    editor.input.focus();
    await flush();
    type('p');
    expect(suggestions()).toEqual(['Parkway Drive', 'Pantera']);
    type('PA');
    expect(suggestions()).toEqual(['Parkway Drive', 'Pantera']);
    type('par');
    expect(suggestions()).toEqual(['Parkway Drive']);
    type('c'); // "clean" is already on the rig
    expect(suggestions()).toEqual([]);
    type('');
    expect(suggestions()).toEqual([]);
  });

  it('arrow keys pick a suggestion and Enter adds it', async () => {
    mount([]);
    editor.input.focus();
    await flush();
    type('pa');
    expect(active()).toBeUndefined();
    key('ArrowDown');
    expect(active()).toBe('Parkway Drive');
    key('ArrowDown');
    expect(active()).toBe('Pantera');
    key('ArrowDown'); // stays on the last one
    key('ArrowUp');
    expect(active()).toBe('Parkway Drive');
    key('Enter');
    expect(onChange).toHaveBeenLastCalledWith(['Parkway Drive']);
    expect(suggestions()).toEqual([]);
  });

  it('clicking a suggestion adds it', async () => {
    mount([]);
    editor.input.focus();
    await flush();
    type('7');
    document.querySelector<HTMLElement>('.tag-suggestion')!.dispatchEvent(new MouseEvent('mousedown', { bubbles: true, cancelable: true }));
    expect(onChange).toHaveBeenLastCalledWith(['70s']);
  });

  it('setTags shows the given list without reporting a change and keeps the typed text', () => {
    mount(['clean']);
    type('wil');
    editor.setTags(['clean', 'Wild Eyes']);
    expect(chips()).toEqual(['clean', 'Wild Eyes']);
    expect(editor.input.value).toBe('wil');
    expect(onChange).not.toHaveBeenCalled();
  });
});
