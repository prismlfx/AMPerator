// Transport between the page and the engine (docs/bridge-protocol.md).
//
// The app's WebBrowserComponent (app/MainWindow.cpp) registers two native functions:
//   amp(json)      forwards one command envelope to amp::Bridge::handle. The function itself completes at once;
//                  the bridge's reply arrives as an "amp" event ({"v":1,"reply":id,...}) like every other event.
//   chooseFolder() opens a native folder chooser and resolves with the chosen path ("" when cancelled).
// Native functions are invoked with JUCE's own low-level protocol (the same one JUCE's index.js uses):
// emitEvent("__juce__invoke", {name, params, resultId}) -> event "__juce__complete" {promiseId, result}.

type Listener = (data: any) => void;
type ListenerToken = [string, number];

interface JuceBackend {
  addEventListener(eventId: string, fn: (payload: any) => void): ListenerToken;
  removeEventListener(token: ListenerToken): void;
  emitEvent(eventId: string, payload: unknown): void;
}

declare global {
  interface Window {
    __JUCE__?: { backend?: JuceBackend; initialisationData?: Record<string, unknown> };
  }
}

export const PROTOCOL_VERSION = 1;
export const CALL_TIMEOUT_MS = 10_000;

interface PendingCall {
  resolve: (v: any) => void;
  reject: (e: Error) => void;
  timer: ReturnType<typeof setTimeout>;
}

const pendingCalls = new Map<number, PendingCall>();
const pendingNative = new Map<number, (result: unknown) => void>();
const listeners = new Map<string, Set<Listener>>();
let nextCallId = 1;
let nextNativeId = 1;
let attached = false;

function backend(): JuceBackend | undefined {
  return window.__JUCE__?.backend;
}

/** Subscribes to the backend once it exists (the page may load before JUCE injects it in dev mode). */
function attach(): JuceBackend | undefined {
  const b = backend();
  if (b && !attached) {
    attached = true;
    b.addEventListener('amp', onBridgeMessage);
    b.addEventListener('__juce__complete', onNativeComplete);
  }
  return b;
}

function onNativeComplete(payload: { promiseId: number; result: unknown }) {
  const resolve = pendingNative.get(payload?.promiseId);
  if (resolve) {
    pendingNative.delete(payload.promiseId);
    resolve(payload.result);
  }
}

function onBridgeMessage(msg: any) {
  if (msg == null || typeof msg !== 'object') return;
  if ('reply' in msg) {
    const p = pendingCalls.get(msg.reply);
    if (!p) return; // timed out already, or not ours
    pendingCalls.delete(msg.reply);
    clearTimeout(p.timer);
    if (msg.ok) p.resolve(msg.result ?? {});
    else p.reject(new Error(String(msg.error ?? 'unknown error')));
  } else if (typeof msg.event === 'string') {
    for (const fn of [...(listeners.get(msg.event) ?? [])]) {
      try {
        fn(msg.data);
      } catch (e) {
        console.error(`listener for "${msg.event}" threw`, e);
      }
    }
  }
}

/** Calls a native function registered with WebBrowserComponent::Options::withNativeFunction. */
export function invokeNative(name: string, ...params: unknown[]): Promise<unknown> {
  const b = attach();
  if (!b) return Promise.reject(new Error('not running inside AMPerator (no native backend)'));
  const resultId = nextNativeId++;
  const result = new Promise<unknown>((resolve) => pendingNative.set(resultId, resolve));
  b.emitEvent('__juce__invoke', { name, params, resultId });
  return result;
}

/** Sends a bridge command; resolves with its `result`, rejects with its `error` or after `timeoutMs` (default 10 s)
    without a reply. Commands that block the engine for long (rescanPlugins) pass a longer timeout. */
export function call<T = any>(cmd: string, args?: object, timeoutMs = CALL_TIMEOUT_MS): Promise<T> {
  if (!attach()) return Promise.reject(new Error('not running inside AMPerator (no native backend)'));
  const id = nextCallId++;
  const envelope: Record<string, unknown> = { v: PROTOCOL_VERSION, cmd, id };
  if (args !== undefined) envelope.args = args;

  return new Promise<T>((resolve, reject) => {
    const timer = setTimeout(() => {
      pendingCalls.delete(id);
      reject(new Error(`${cmd}: no reply from the engine within ${timeoutMs / 1000} s`));
    }, timeoutMs);
    pendingCalls.set(id, { resolve, reject, timer });
    invokeNative('amp', JSON.stringify(envelope)).catch(() => {}); // the reply comes as an "amp" event
  });
}

/** Listens for a bridge event (`rigLoaded`, `deviceStatus`, ...). Returns an unsubscribe function. */
export function on(event: string, fn: (data: any) => void): () => void {
  attach();
  if (!listeners.has(event)) listeners.set(event, new Set());
  const set = listeners.get(event)!;
  const wrapped: Listener = (d) => fn(d); // the same fn may be registered twice; each gets its own entry
  set.add(wrapped);
  return () => set.delete(wrapped);
}
