// In-page modals. WKWebView as hosted by JUCE implements no JavaScript alert/confirm panels, so
// window.confirm() cannot be used: everything modal is drawn by the page.

export interface Modal {
  root: HTMLElement;
  body: HTMLElement;
  close(): void;
}

/** Element helper: el('div', {class: 'x'}, 'text', child). */
export function el<K extends keyof HTMLElementTagNameMap>(
  tag: K,
  attrs: Record<string, string> = {},
  ...children: (Node | string)[]
): HTMLElementTagNameMap[K] {
  const e = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) e.setAttribute(k, v);
  e.append(...children);
  return e;
}

export function openModal(title: string, onClose?: () => void, extraClass = ''): Modal {
  const closeBtn = el('button', { class: 'icon-button', 'aria-label': 'Close', title: 'Close' }, '×');
  const body = el('div', { class: 'modal-body' });
  const box = el(
    'div',
    { class: `modal ${extraClass}`.trim(), role: 'dialog', 'aria-modal': 'true', 'aria-label': title },
    el('header', { class: 'modal-header' }, el('h2', {}, title), closeBtn),
    body,
  );
  const root = el('div', { class: 'modal-backdrop' }, box);

  let closed = false;
  const close = () => {
    if (closed) return;
    closed = true;
    document.removeEventListener('keydown', onKey);
    root.remove();
    onClose?.();
  };
  const onKey = (e: KeyboardEvent) => {
    if (e.key === 'Escape') close();
  };
  closeBtn.addEventListener('click', close);
  root.addEventListener('mousedown', (e) => {
    if (e.target === root) close();
  });
  document.addEventListener('keydown', onKey);
  document.body.append(root);
  return { root, body, close };
}

export function confirmDialog(message: string, okLabel = 'OK', cancelLabel = 'Cancel'): Promise<boolean> {
  return new Promise((resolve) => {
    let answer = false;
    const modal = openModal('Confirm', () => resolve(answer), 'modal-small');
    const ok = el('button', { class: 'button primary' }, okLabel);
    const cancel = el('button', { class: 'button' }, cancelLabel);
    ok.addEventListener('click', () => {
      answer = true;
      modal.close();
    });
    cancel.addEventListener('click', () => modal.close());
    modal.body.append(el('p', {}, message), el('div', { class: 'modal-actions' }, cancel, ok));
    ok.focus();
  });
}

/** Transient notice in the bottom-right corner (errors stay a little longer). */
export function toast(message: string, kind: 'error' | 'info' = 'error', ms = kind === 'error' ? 6000 : 3000) {
  let host = document.getElementById('toasts');
  if (!host) {
    host = el('div', { id: 'toasts', 'aria-live': 'polite' });
    document.body.append(host);
  }
  const close = el('button', { class: 'icon-button', 'aria-label': 'Dismiss' }, '×');
  const t = el('div', { class: `toast toast-${kind}`, role: kind === 'error' ? 'alert' : 'status' }, el('span', {}, message), close);
  const remove = () => t.remove();
  close.addEventListener('click', remove);
  setTimeout(remove, ms);
  host.append(t);
}
