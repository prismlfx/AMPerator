// Rig actions of the top bar that talk to the engine: Save as… (a copy under a new id) and the quit prompt the engine
// asks for when the app is quit with unsaved changes (docs/bridge-protocol.md: quitRequested / confirmQuit).

import { call, on } from './bridge';
import { choiceDialog, promptDialog, toast } from './dialog';
import { store } from './state';

export type QuitChoice = 'save' | 'discard' | 'cancel';

function askQuit(rigName: string): Promise<QuitChoice> {
  return choiceDialog<QuitChoice>(
    'Quit AMPerator',
    `“${rigName || 'This rig'}” has unsaved changes. Save them before quitting?`,
    [
      { label: 'Cancel', value: 'cancel' },
      { label: 'Discard', value: 'discard', danger: true },
      { label: 'Save', value: 'save', primary: true },
    ],
    'cancel',
  );
}

/** Answers every `quitRequested` with confirmQuit{action}. Returns an unsubscribe function. */
export function watchQuitRequests(ask: (rigName: string) => Promise<QuitChoice> = askQuit): () => void {
  return on('quitRequested', async (d) => {
    const action = await ask(String(d?.rigName ?? store.get().rig?.name ?? ''));
    try {
      await call('confirmQuit', { action });
    } catch (e) {
      toast(`Couldn't save: ${(e as Error).message}`);
    }
  });
}

type SaveAsPrompt = (initial: { name: string; band: string }) => Promise<{ name: string; band: string } | null>;

const promptSaveAs: SaveAsPrompt = (initial) =>
  promptDialog(
    'Save as',
    [
      { key: 'name', label: 'Name', value: initial.name },
      { key: 'band', label: 'Band', value: initial.band },
    ],
    'Save copy',
  );

/** Saves the current rig as a new rig (fresh id; the original file stays as it is). The copy becomes the current rig. */
export async function saveRigAs(ask: SaveAsPrompt = promptSaveAs): Promise<void> {
  const rig = store.get().rig;
  if (!rig) return;
  const answer = await ask({ name: `${rig.name} copy`, band: rig.band });
  if (!answer) return;
  try {
    const r = await call<{ id: string; name: string; band: string }>('saveRig', { asNew: true, name: answer.name, band: answer.band });
    toast(`Saved “${r.name}”`, 'info');
  } catch (e) {
    toast(`Couldn't save: ${(e as Error).message}`);
  }
}

/** Re-enables a plugin that was disabled after a crash. Resolves true on success. */
export async function retryBlacklistedPlugin(id: string): Promise<boolean> {
  try {
    await call('unblacklistPlugin', { id });
    toast('Plugin re-enabled. Rescan plugins if it does not show up.', 'info', 6000);
    return true;
  } catch (e) {
    toast((e as Error).message);
    return false;
  }
}
