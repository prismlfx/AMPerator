// Rotary knob: drag vertically to change (Shift = fine), wheel, arrow keys, double-click resets to the default.
// Shows its label, value and unit; exposes the ARIA slider role.

import { el } from '../dialog';

export interface KnobOptions {
  label: string;
  min: number;
  max: number;
  value: number;
  defaultValue: number;
  unit: string;
  /** Called on every user change (already clamped). */
  onChange: (value: number) => void;
}

export interface Knob {
  el: HTMLElement;
  /** Shows a value from the engine; ignored while the user is dragging this knob. */
  setValue(value: number): void;
}

const DRAG_PIXELS = 200; // full range per 200 px of vertical drag (x10 with Shift)
const SWEEP_DEG = 270;

/** Calls `fn` at most once per `ms`; the last call within a window is always delivered (trailing call). */
export function throttle<A extends unknown[]>(fn: (...args: A) => void, ms: number): (...args: A) => void {
  let last = -Infinity;
  let timer: ReturnType<typeof setTimeout> | null = null;
  let pending: A | null = null;
  const fire = (args: A) => {
    last = Date.now();
    fn(...args);
  };
  return (...args: A) => {
    const wait = last + ms - Date.now();
    if (wait <= 0 && timer === null) {
      fire(args);
      return;
    }
    pending = args;
    if (timer === null)
      timer = setTimeout(() => {
        timer = null;
        if (pending) {
          const a = pending;
          pending = null;
          fire(a);
        }
      }, Math.max(wait, 0));
  };
}

export function formatValue(value: number, unit: string): string {
  const abs = Math.abs(value);
  if (unit === 'Hz' && abs >= 1000) return `${(value / 1000).toFixed(abs >= 10000 ? 1 : 2)} kHz`;
  return `${abs >= 100 ? value.toFixed(0) : value.toFixed(1)} ${unit}`;
}

export function createKnob(o: KnobOptions): Knob {
  const range = o.max - o.min;
  const clamp = (v: number) => Math.min(o.max, Math.max(o.min, v));
  let value = clamp(o.value);
  let dragging = false;

  const indicator = el('span', { class: 'knob-indicator' });
  const dial = el('span', { class: 'knob-dial', 'aria-hidden': 'true' }, indicator);
  const valueText = el('span', { class: 'knob-value' });
  const root = el(
    'div',
    {
      class: 'knob',
      role: 'slider',
      tabindex: '0',
      'aria-label': o.label,
      'aria-valuemin': String(o.min),
      'aria-valuemax': String(o.max),
      title: `${o.label} — drag up/down (Shift: fine), double-click to reset`,
    },
    dial,
    valueText,
    el('span', { class: 'knob-label' }, o.label),
  );

  const show = () => {
    const text = formatValue(value, o.unit);
    valueText.textContent = text;
    root.setAttribute('aria-valuenow', String(Math.round(value * 100) / 100));
    root.setAttribute('aria-valuetext', text);
    dial.style.setProperty('--angle', `${((value - o.min) / range - 0.5) * SWEEP_DEG}deg`);
  };

  const change = (v: number) => {
    const next = clamp(v);
    if (next === value) return;
    value = next;
    show();
    o.onChange(value);
  };

  root.addEventListener('pointerdown', (e) => {
    if (e.button !== 0) return;
    e.preventDefault();
    e.stopPropagation(); // not a card drag
    root.focus();
    dragging = true;
    let lastY = e.clientY;
    const move = (m: PointerEvent) => {
      const dy = lastY - m.clientY;
      lastY = m.clientY;
      change(value + (dy / (m.shiftKey ? DRAG_PIXELS * 10 : DRAG_PIXELS)) * range);
    };
    const up = () => {
      dragging = false;
      window.removeEventListener('pointermove', move);
      window.removeEventListener('pointerup', up);
      window.removeEventListener('pointercancel', up);
    };
    window.addEventListener('pointermove', move);
    window.addEventListener('pointerup', up);
    window.addEventListener('pointercancel', up);
  });
  root.addEventListener('dblclick', (e) => {
    e.stopPropagation();
    change(o.defaultValue);
  });
  root.addEventListener(
    'wheel',
    (e) => {
      e.preventDefault();
      const steps = -Math.sign(e.deltaY);
      change(value + steps * range * (e.shiftKey ? 0.002 : 0.02));
    },
    { passive: false },
  );
  root.addEventListener('keydown', (e) => {
    const step = range * (e.shiftKey ? 0.002 : 0.02);
    const keys: Record<string, () => number> = {
      ArrowUp: () => value + step,
      ArrowRight: () => value + step,
      ArrowDown: () => value - step,
      ArrowLeft: () => value - step,
      Home: () => o.min,
      End: () => o.max,
    };
    if (keys[e.key]) {
      e.preventDefault();
      change(keys[e.key]());
    }
  });

  show();
  return {
    el: root,
    setValue(v: number) {
      if (dragging) return;
      value = clamp(v);
      show();
    },
  };
}
