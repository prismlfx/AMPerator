#pragma once

// Where a downloaded TONE3000 file lives in the library and whether its content is what the tone claims
// (tone-library spec section 7.1). Pure file helpers; message thread or worker, no shared state.

#include <juce_core/juce_core.h>

#include "ProviderIndex.h"

namespace amp::online
{
// "" when the file is acceptable. format "nam": a JSON object with a string "version"; anything else ("ir"): a WAV or
// AIFF that opens with at least one channel and one sample (read through a reader, never loaded whole).
// Otherwise "not a valid <format> file".
juce::String validateAssetFile(const juce::File&, const juce::String& format);

// <root>/<Models|IRs>/TONE3000/<creator>/<title> — <model name>.<nam|wav>. Each of creator, title and model name goes
// through RigStore::sanitizeFileName and is cut to at most 80 characters (and 100 UTF-8 bytes, to stay inside file
// system name limits). An empty creator becomes "unknown". Format "nam" -> Models/.nam, anything else -> IRs/.wav.
juce::File placementFor(const juce::File& libraryRoot, const Provenance&);

// Moves `partial` to `target`, creating directories. An existing file with the same sha256 is reused (the partial is
// deleted); a different one makes the name " (2)", " (3)", ... Never overwrites. Returns the final file, or
// juce::File() with `error` set (the partial is left where it is).
juce::File placeFile(const juce::File& partial, const juce::File& target, const juce::String& sha256,
                     juce::String& error);
} // namespace amp::online
