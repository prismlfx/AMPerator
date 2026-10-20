#include "MicrophonePermission.h"

#if ! defined(__APPLE__) // macOS: MicrophonePermission.mm
 #include <juce_core/juce_core.h>

namespace amp
{
void requestMicrophoneAccess(std::function<void(bool)> done)
{
    juce::RuntimePermissions::request(juce::RuntimePermissions::recordAudio, std::move(done));
}
} // namespace amp
#endif
