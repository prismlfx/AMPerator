// Test double for the backend object JUCE's WebBrowserComponent injects as window.__JUCE__.backend
// (same listener-token and __juce__invoke / __juce__complete semantics as juce_WebBrowserComponent.cpp).

type Listener = (payload: any) => void;

export interface FakeJuce {
  /** Every emitEvent(eventId, payload) the page sent to the backend. */
  sent: { eventId: string; payload: any }[];
  /** Delivers an event from the backend to the page (payload JSON round-tripped, as JUCE does). */
  fromBackend(eventId: string, payload: unknown): void;
  /** The bridge commands sent so far (parsed `amp` native-function arguments). */
  commands(): any[];
  /** Replies to a bridge command the way amp::Bridge does. */
  reply(id: number, ok: boolean, resultOrError: unknown): void;
  listenerCount(eventId: string): number;
}

export function installFakeJuce(): FakeJuce {
  const listeners = new Map<string, Map<number, Listener>>();
  let nextId = 0;
  const sent: { eventId: string; payload: any }[] = [];

  const backend = {
    addEventListener(eventId: string, fn: Listener): [string, number] {
      if (!listeners.has(eventId)) listeners.set(eventId, new Map());
      const id = nextId++;
      listeners.get(eventId)!.set(id, fn);
      return [eventId, id];
    },
    removeEventListener([eventId, id]: [string, number]) {
      listeners.get(eventId)?.delete(id);
    },
    emitEvent(eventId: string, payload: unknown) {
      sent.push({ eventId, payload: JSON.parse(JSON.stringify(payload)) });
    },
  };

  (window as any).__JUCE__ = {
    backend,
    initialisationData: { __juce__functions: ['amp', 'chooseFolder'] },
  };

  const fromBackend = (eventId: string, payload: unknown) => {
    for (const fn of [...(listeners.get(eventId)?.values() ?? [])]) fn(JSON.parse(JSON.stringify(payload)));
  };

  return {
    sent,
    fromBackend,
    commands: () =>
      sent
        .filter((s) => s.eventId === '__juce__invoke' && s.payload.name === 'amp')
        .map((s) => JSON.parse(s.payload.params[0])),
    reply(id, ok, resultOrError) {
      fromBackend('amp', ok ? { v: 1, reply: id, ok: true, result: resultOrError } : { v: 1, reply: id, ok: false, error: resultOrError });
    },
    listenerCount: (eventId) => listeners.get(eventId)?.size ?? 0,
  };
}
