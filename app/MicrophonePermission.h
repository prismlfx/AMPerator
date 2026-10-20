#pragma once

#include <functional>

namespace amp
{
/** Asks the OS for microphone (audio input) access without blocking: `done(granted)` is called later on the message
    thread. On macOS this goes through AVCaptureDevice (juce::RuntimePermissions only implements Android and always
    answers "granted" elsewhere), so the TCC prompt is shown asynchronously instead of blocking the message thread
    inside CoreAudio when the device opens. Other platforms: granted. */
void requestMicrophoneAccess(std::function<void(bool granted)> done);
} // namespace amp
