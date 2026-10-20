#pragma once

#include <juce_core/juce_core.h>

#include <map>
#include <vector>

#include "model/RigDocument.h"

namespace amp
{
struct RigEntry { juce::String id, name, band; juce::File file; };

/** On-disk rig library: <root>/Models, IRs, Rigs/<Band>/, Art.
    Rigs are indexed by scanning Rigs/<Band>/(star).amprig and reading each file's `id`.
    Display names/bands come from the JSON, never the file/folder name.
    Corrupt files (unparseable, no id) are skipped by list(). load(id) falls back to the path
    remembered from the last good scan so a rig whose file was later corrupted reports a parse
    error instead of "not found"; nothing ever rewrites a file that failed to parse unless the
    user saves a rig with the same id to that same path (the previous bytes are then rotated into .bak.1). */
class RigStore
{
public:
    static constexpr int maxBackups = 5;

    explicit RigStore(juce::File libraryRoot);

    juce::File root() const;
    std::vector<RigEntry> list() const;   // sorted band then name (case-insensitive)
    juce::StringArray bands() const;      // folders under Rigs/ plus "My Tones", sorted
    ParseResult load(const juce::String& rigId) const;
    juce::Result save(const RigDocument&);
    juce::Result remove(const juce::String& rigId);
    std::vector<juce::File> backups(const juce::String& rigId) const; // newest first

    static juce::String sanitizeFileName(const juce::String&);
    static juce::File defaultLibraryRoot();

private:
    juce::File rigsDir() const;
    juce::File fileFor(const juce::String& rigId) const;
    static juce::File backupFile(const juce::File& rig, int n);
    static bool rotateBackups(const juce::File& rig);
    static void moveBackups(const juce::File& from, const juce::File& to);
    static void deleteBackups(const juce::File& rig);

    juce::File root_;
    mutable std::map<juce::String, juce::File> knownPaths_; // id -> file, from last good scans
};
} // namespace amp
