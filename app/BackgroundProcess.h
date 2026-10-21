#pragma once

namespace amp
{
/** Keeps this process out of the Dock and the Cmd-Tab switcher (NSApplicationActivationPolicyProhibited). Must run
    before NSApplication is created, i.e. before JUCEApplicationBase::main. No-op off macOS. */
void runAsBackgroundProcess();
} // namespace amp
