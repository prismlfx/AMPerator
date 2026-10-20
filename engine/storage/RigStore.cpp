#include "storage/RigStore.h"

#include <algorithm>

namespace amp
{
namespace
{
const char* const kDefaultBand = "My Tones";

struct Scanned { std::optional<RigDocument> rig; juce::File file; };

std::vector<Scanned> scanRigs(const juce::File& rigsDir)
{
    std::vector<Scanned> out;
    for (auto& bandDir : rigsDir.findChildFiles(juce::File::findDirectories, false))
        for (auto& f : bandDir.findChildFiles(juce::File::findFiles, false, "*.amprig"))
            out.push_back({ parseRig(f.loadFileAsString()).rig, f });
    return out;
}

bool writeAtomically(const juce::File& target, const juce::String& text)
{
    juce::TemporaryFile tmp(target, target.getSiblingFile(target.getFileName() + ".tmp"));
    return tmp.getFile().replaceWithText(text) && tmp.overwriteTargetFileWithTemporary();
}
} // namespace

RigStore::RigStore(juce::File libraryRoot) : root_(std::move(libraryRoot))
{
    for (auto* sub : { "Models", "IRs", "Rigs", "Art" })
        root_.getChildFile(sub).createDirectory();
}

juce::File RigStore::root() const { return root_; }
juce::File RigStore::rigsDir() const { return root_.getChildFile("Rigs"); }

juce::File RigStore::backupFile(const juce::File& rig, int n)
{
    return rig.getSiblingFile(rig.getFileName() + ".bak." + juce::String(n));
}

void RigStore::rotateBackups(const juce::File& rig)
{
    backupFile(rig, maxBackups).deleteFile();
    for (int i = maxBackups - 1; i >= 1; --i)
        if (auto b = backupFile(rig, i); b.existsAsFile())
            b.moveFileTo(backupFile(rig, i + 1));
    rig.copyFileTo(backupFile(rig, 1));
}

void RigStore::moveBackups(const juce::File& from, const juce::File& to)
{
    for (int i = 1; i <= maxBackups; ++i)
    {
        auto src = backupFile(from, i);
        if (!src.existsAsFile())
            continue;
        auto dst = backupFile(to, i);
        dst.deleteFile();
        src.moveFileTo(dst);
    }
}

void RigStore::deleteBackups(const juce::File& rig)
{
    for (int i = 1; i <= maxBackups; ++i)
        backupFile(rig, i).deleteFile();
}

juce::String RigStore::sanitizeFileName(const juce::String& name)
{
    juce::String out;
    for (auto c : name)
        out += (c < 32 || juce::String("/\\:*?\"<>|").containsChar(c)) ? juce::juce_wchar('_') : c;
    out = out.trim();
    out = out.trimCharactersAtStart(". ").trimCharactersAtEnd(". ");
    if (out.length() > 120)
        out = out.substring(0, 120).trimCharactersAtEnd(". ");
    return out.isEmpty() ? juce::String("Untitled") : out;
}

juce::File RigStore::defaultLibraryRoot()
{
   #if JUCE_WINDOWS
    return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("AMPerator");
   #else
    return juce::File::getSpecialLocation(juce::File::userMusicDirectory).getChildFile("AMPerator");
   #endif
}

std::vector<RigEntry> RigStore::list() const
{
    std::vector<RigEntry> out;
    for (auto& s : scanRigs(rigsDir()))
    {
        if (!s.rig || s.rig->id.isEmpty())
            continue;
        knownPaths_[s.rig->id] = s.file;
        out.push_back({ s.rig->id, s.rig->name, s.rig->band, s.file });
    }
    std::sort(out.begin(), out.end(), [](const RigEntry& a, const RigEntry& b)
    {
        if (int c = a.band.compareIgnoreCase(b.band))
            return c < 0;
        if (int c = a.name.compareIgnoreCase(b.name))
            return c < 0;
        return a.file.getFullPathName() < b.file.getFullPathName();
    });
    return out;
}

juce::StringArray RigStore::bands() const
{
    juce::StringArray b;
    b.add(kDefaultBand);
    for (auto& e : list())
        b.addIfNotAlreadyThere(e.band);
    for (auto& d : rigsDir().findChildFiles(juce::File::findDirectories, false))
        b.addIfNotAlreadyThere(d.getFileName());
    b.sortNatural();
    return b;
}

juce::File RigStore::fileFor(const juce::String& rigId) const
{
    for (auto& e : list())
        if (e.id == rigId)
            return e.file;
    auto it = knownPaths_.find(rigId);
    return it != knownPaths_.end() ? it->second : juce::File();
}

ParseResult RigStore::load(const juce::String& rigId) const
{
    auto f = fileFor(rigId);
    if (f == juce::File())
        return { std::nullopt, "Rig not found: " + rigId };
    if (!f.existsAsFile())
        return { std::nullopt, "Rig file missing: " + f.getFullPathName() };
    return parseRig(f.loadFileAsString());
}

juce::Result RigStore::save(const RigDocument& rig)
{
    if (rig.id.isEmpty())
        return juce::Result::fail("Rig has no id");

    const auto oldFile = fileFor(rig.id);
    const auto bandDir = rigsDir().getChildFile(sanitizeFileName(rig.band));
    const auto base = sanitizeFileName(rig.name);

    // Pick the first path that is free or already owned by this rig. A file that cannot be parsed
    // has an unknown owner, so it is never claimed.
    juce::File target;
    for (int n = 1;; ++n)
    {
        target = bandDir.getChildFile(base + (n == 1 ? juce::String() : " (" + juce::String(n) + ")") + ".amprig");
        if (!target.existsAsFile() || target == oldFile)
            break;
        auto existing = parseRig(target.loadFileAsString()).rig;
        if (existing && existing->id == rig.id)
            break;
    }

    if (!bandDir.createDirectory())
        return juce::Result::fail("Cannot create " + bandDir.getFullPathName());

    if (target.existsAsFile())
        rotateBackups(target);
    if (!writeAtomically(target, serializeRig(rig)))
        return juce::Result::fail("Cannot write " + target.getFullPathName());

    if (oldFile != juce::File() && oldFile != target && oldFile.existsAsFile())
    {
        moveBackups(oldFile, target);
        oldFile.deleteFile();
    }
    knownPaths_[rig.id] = target;
    return juce::Result::ok();
}

juce::Result RigStore::remove(const juce::String& rigId)
{
    auto f = fileFor(rigId);
    if (f == juce::File())
        return juce::Result::fail("Rig not found: " + rigId);
    deleteBackups(f);
    if (f.existsAsFile() && !f.deleteFile())
        return juce::Result::fail("Cannot delete " + f.getFullPathName());
    knownPaths_.erase(rigId);
    return juce::Result::ok();
}

std::vector<juce::File> RigStore::backups(const juce::String& rigId) const
{
    std::vector<juce::File> out;
    auto f = fileFor(rigId);
    if (f == juce::File())
        return out;
    for (int i = 1; i <= maxBackups; ++i)
        if (auto b = backupFile(f, i); b.existsAsFile())
            out.push_back(b);
    return out;
}
} // namespace amp
