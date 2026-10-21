// Keyboard helpers shared by the views' shortcuts.

/** True while a text field (input, textarea, select, contenteditable) has the focus or receives the event: single-key
    shortcuts must not fire then. */
export function isTyping(target: EventTarget | null): boolean {
  return [target, document.activeElement].some(
    // isContentEditable covers inherited editing; the selector is a fallback where it is not implemented (jsdom).
    (x) =>
      x instanceof HTMLElement &&
      (x.isContentEditable || !!x.closest('input, textarea, select, [contenteditable]:not([contenteditable="false"])')),
  );
}
