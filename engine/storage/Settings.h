#pragma once

#include <juce_core/juce_core.h>

#include <cstdint>
#include <map>
#include <optional>
#include <utility>
#include <vector>

#include "model/RigDocument.h"

namespace amp
{
/** Machine-local configuration (settings.json in the library root). */
struct Settings
{
    static constexpr int maxRecent = 10;

    juce::String audioDeviceType, audioDeviceName;
    int inputChannel = 0, outputFirstChannel = 0;
    double sampleRate = 48000;
    int bufferSize = 128;
    juce::StringArray watchedFolders;
    std::vector<Mapping> globalMappings;
    juce::String lastRigId;
    juce::StringArray recentRigIds /* max 10 */, favoriteRigIds;
    /** HID keycode -> footswitch control ID. Default: the PCsensor factory keys a/b/c, plus F13/F14/F15 (pedals
        reprogrammed to these type nothing visible when the device can only be opened shared). */
    std::map<int, juce::String> footswitchKeys { { 0x04, "footswitch:L" }, { 0x05, "footswitch:M" }, { 0x06, "footswitch:R" },
                                                 { 0x68, "footswitch:L" }, { 0x69, "footswitch:M" }, { 0x6A, "footswitch:R" } };

    /** A keyboard-type HID device (vendor id, product id) the user picked as the footswitch, in addition to the
        automatically recognised models. Stored as {"vid": n, "pid": n}. */
    std::optional<std::pair<uint16_t, uint16_t>> footswitchDevice;

    /** Missing/corrupt file -> defaults; a corrupt file is first copied to settings.json.corrupt (.corrupt.2, ... when
        that exists). Missing keys or wrong-typed values -> that field's default. */
    static Settings load(const juce::File&);
    /** Atomic (temp file in the same directory + rename). */
    bool save(const juce::File&) const;

    /** Moves `rigId` to the front, dedupes, trims to maxRecent. */
    void noteRecent(const juce::String& rigId);
};
} // namespace amp
