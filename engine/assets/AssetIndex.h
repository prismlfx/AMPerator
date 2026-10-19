#pragma once

#include "model/RigDocument.h"

#include <juce_core/juce_core.h>

#include <map>
#include <vector>

namespace amp
{
struct AssetEntry { juce::File file; juce::String kind, sha256; juce::int64 size = 0, modTime = 0; };

enum class ResolveKind { ByHash, ByFilename, Missing };
struct Resolved { ResolveKind how = ResolveKind::Missing; juce::File file; };

/** Indexes .nam ("nam") and .wav/.aiff ("ir") files under a set of root folders by content hash,
    so rig asset references survive renames and moves. Not thread-safe: call from one thread at a
    time (rescan() does file I/O and hashing, so run it on a background thread). */
class AssetIndex
{
public:
    void setRoots(juce::Array<juce::File> roots);
    /** Recursive scan; skips hidden files and files in hidden directories. Hashes are cached by
        (path, size, modTime), so unchanged files are not re-read. */
    void rescan();
    /** Hash match (same kind) first, then case-insensitive file-name match (same kind, first in
        sorted path order), else Missing. */
    Resolved resolve(const AssetRef&) const;
    std::vector<AssetEntry> list(const juce::String& kind) const;
    /** Lowercase hex SHA-256 of the file contents; empty string if the file can't be read. */
    static juce::String sha256Of(const juce::File&);

    /** Test support: total number of files hashed by rescan() since construction. */
    int hashCount() const { return hashes; }

private:
    juce::Array<juce::File> roots;
    std::vector<AssetEntry> entries; // sorted by full path
    std::map<juce::String, AssetEntry> cache; // by full path
    int hashes = 0;
};
} // namespace amp
