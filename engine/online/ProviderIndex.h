#pragma once

// Provenance of downloaded TONE3000 files, keyed by the file's sha256 (tone-library spec section 7.2).
// Only metadata of files the user downloaded is stored; this is attribution, not a catalog cache.

#include <map>
#include <optional>

#include <juce_core/juce_core.h>

#include "Tone3000Types.h"

namespace amp::online
{
struct Provenance
{
    juce::String provider { "tone3000" }, modelId, toneId, title, modelName, size, architecture, format, gear, license,
        toneUrl,
        image /* path relative to Art/, e.g. "tone3000/t_456.jpg", or "" */,
        downloadedAt /* ISO 8601 UTC */;
    UserRef creator;
};

// The bridge `source` object: {provider, id: modelId, toneId, title, creator, license, url: toneUrl, image}.
// `image` stays the path relative to Art/; a later layer turns it into an absolute path.
juce::var toVar(const Provenance&);

// Not thread-safe: message thread only.
class ProviderIndex
{
public:
    explicit ProviderIndex(juce::File indexFile); // <dataDir>/tone3000/index.json

    // Reads the file; creates nothing on disk. A missing file gives an empty index; an unreadable one is renamed to
    // index.json.corrupt (.corrupt.2, ... when that exists) and the index is empty.
    void load();
    // Atomic write; creates the parent directory.
    bool save() const;

    void put(const juce::String& sha256, Provenance); // the key is stored lower-case
    std::optional<Provenance> bySha(const juce::String& sha256) const;
    // With several files for one model id, the one with the newest downloadedAt.
    std::optional<juce::String> shaForModel(const juce::String& modelId) const;
    size_t size() const;

private:
    juce::File file;
    std::map<juce::String, Provenance> entries;
    bool keepFileOnSave = false; // an unreadable file could not be set aside; save() refuses to replace it
};
} // namespace amp::online
