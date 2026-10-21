#pragma once

#include <juce_core/juce_core.h>

#include <functional>
#include <map>
#include <vector>

#include "model/RigDocument.h"

namespace amp
{
struct RigEntry
{
    juce::String id, name, band;
    juce::File file;
    juce::StringArray tags; // as stored in the file
};

/** On-disk rig library: <root>/Models, IRs, Rigs/<Band>/, Art.
    Rigs are indexed by scanning Rigs/<Band>/(star).amprig and reading each file's `id`.
    Display names/bands come from the JSON, never the file/folder name.
    Corrupt files (unparseable, no id) are skipped by list(). load(id) falls back to the path
    remembered from the last good scan, else to the rig file named by a parseable backup
    (<rig>.amprig.bak.N) with that id -- so a rig whose file was corrupted, even before this process
    started, reports a parse error instead of "not found" and backups(id) still finds its backups; nothing ever rewrites a file that failed to parse unless the
    user saves a rig with the same id to that same path (the previous bytes are then rotated into .bak.1).
    Two files with the same id (e.g. a Finder copy): list() keeps the id for the file it already knew (else the oldest
    file, then the shortest name) and rewrites the other(s) with a fresh id, so every file lists and opens on its own.
    Hidden files and folders (.DS_Store, ...) are ignored. */
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
    /** Hands the rig file and its backups to the trash function (default: juce::File::moveToTrash). */
    juce::Result remove(const juce::String& rigId);
    /** Replaces how remove() disposes of files (tests: never the user's Trash). Returns false on failure. */
    void setTrashFunction(std::function<bool(const juce::File&)>);
    std::vector<juce::File> backups(const juce::String& rigId) const; // newest first

    static juce::String sanitizeFileName(const juce::String&);
    static juce::File defaultLibraryRoot();

private:
    juce::File rigsDir() const;
    juce::File fileFor(const juce::String& rigId) const;
    static juce::File backupFile(const juce::File& rig, int n);
    static bool rotateBackups(const juce::File& rig);
    /** Shifts rig's backups up by one (dropping the oldest) and copies `source` into backup 1. */
    static bool pushBackup(const juce::File& rig, const juce::File& source);
    void resolveDuplicateIds(std::vector<RigEntry>&) const;
    static void moveBackups(const juce::File& from, const juce::File& to);

    juce::File root_;
    std::function<bool(const juce::File&)> trash_ = [](const juce::File& f) { return f.moveToTrash(); };
    mutable std::map<juce::String, juce::File> knownPaths_; // id -> file, from last good scans
};
} // namespace amp
