#include "storage/RigStore.h"

#include <algorithm>
#include <tuple>

#include "storage/AtomicFile.h"

namespace amp
{
namespace
{
const char* const kDefaultBand = "My Tones";

struct Scanned { std::optional<RigDocument> rig; juce::File file; };

constexpr int kDirs = juce::File::findDirectories | juce::File::ignoreHiddenFiles;
constexpr int kFiles = juce::File::findFiles | juce::File::ignoreHiddenFiles;

std::vector<Scanned> scanRigs(const juce::File& rigsDir)
{
    std::vector<Scanned> out;
    for (auto& bandDir : rigsDir.findChildFiles(kDirs, false))
        for (auto& f : bandDir.findChildFiles(kFiles, false, "*.amprig"))
            out.push_back({ parseRig(f.loadFileAsString()).rig, f });
    return out;
}

// True when touching `f` on behalf of `rigId` cannot destroy another rig's data.
bool notForeign(const juce::File& f, const juce::String& rigId)
{
    if (!f.existsAsFile())
        return true;
    auto parsed = parseRig(f.loadFileAsString()).rig;
    return !parsed || parsed->id.isEmpty() || parsed->id == rigId;
}

// Removes a band folder that holds nothing but hidden files (.DS_Store and the like).
void pruneIfEmpty(const juce::File& dir)
{
    if (! dir.isDirectory()
        || dir.getNumberOfChildFiles(juce::File::findFilesAndDirectories | juce::File::ignoreHiddenFiles) != 0)
        return;
    for (auto& hidden : dir.findChildFiles(juce::File::findFiles, false))
        hidden.deleteFile();
    dir.deleteFile(); // fails (keeps the folder) if a hidden folder is left inside
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

bool RigStore::rotateBackups(const juce::File& rig) { return pushBackup(rig, rig); }

bool RigStore::pushBackup(const juce::File& rig, const juce::File& source)
{
    backupFile(rig, maxBackups).deleteFile();
    for (int i = maxBackups - 1; i >= 1; --i)
        if (auto b = backupFile(rig, i); b.existsAsFile() && !b.moveFileTo(backupFile(rig, i + 1)))
            return false;
    return source.copyFileTo(backupFile(rig, 1));
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
        out.push_back({ s.rig->id, s.rig->name, s.rig->band, s.file, s.rig->tags });
    }
    resolveDuplicateIds(out);
    for (auto& e : out)
        knownPaths_[e.id] = e.file;
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

void RigStore::resolveDuplicateIds(std::vector<RigEntry>& entries) const
{
    std::map<juce::String, std::vector<size_t>> byId;
    for (size_t i = 0; i < entries.size(); ++i)
        byId[entries[i].id].push_back(i);
    for (auto& [id, indices] : byId)
    {
        if (indices.size() < 2)
            continue;
        // The owner: the file this store already knew for the id, else the oldest file, else the shortest name
        // (a Finder copy is "<name> copy.amprig" / "<name> 2.amprig"), else path order.
        const auto known = knownPaths_.find(id);
        auto rank = [&](size_t i)
        {
            const auto& f = entries[i].file;
            return std::make_tuple(known != knownPaths_.end() && known->second == f ? 0 : 1,
                                   f.getCreationTime().toMilliseconds(), f.getFileName().length(), f.getFullPathName());
        };
        std::sort(indices.begin(), indices.end(), [&](size_t a, size_t b) { return rank(a) < rank(b); });
        for (size_t k = 1; k < indices.size(); ++k)
        {
            auto& e = entries[indices[k]];
            auto parsed = parseRig(e.file.loadFileAsString()).rig;
            if (!parsed)
                continue;
            parsed->id = juce::Uuid().toString();
            if (writeFileAtomically(e.file, serializeRig(*parsed)))
                e.id = parsed->id;
        }
    }
    // A rewrite that failed leaves the duplicate listed under the owner's id: drop it rather than list one id twice.
    std::map<juce::String, int> seen;
    std::erase_if(entries, [&](const RigEntry& e) { return seen[e.id]++ > 0; });
}

juce::StringArray RigStore::bands() const
{
    juce::StringArray b;
    b.add(kDefaultBand);
    for (auto& e : list())
        b.addIfNotAlreadyThere(e.band);
    // Folder names only count for user-created folders that hold no rigs (a rig's band comes from its JSON).
    for (auto& d : rigsDir().findChildFiles(kDirs, false))
        if (d.findChildFiles(kFiles, false, "*.amprig").isEmpty())
            b.addIfNotAlreadyThere(d.getFileName());
    b.sortNatural();
    return b;
}

juce::File RigStore::fileFor(const juce::String& rigId) const
{
    for (auto& e : list())
        if (e.id == rigId)
            return e.file;
    if (auto it = knownPaths_.find(rigId); it != knownPaths_.end())
        return it->second;
    // A rig whose file became unreadable before this process ever saw it: its backups still carry the id
    // (newest first), and the rig file is the backup's name minus ".bak.N".
    for (auto& band : rigsDir().findChildFiles(juce::File::findDirectories, false))
        for (int n = 1; n <= maxBackups; ++n)
            for (auto& b : band.findChildFiles(juce::File::findFiles, false, "*.amprig.bak." + juce::String(n)))
                if (auto parsed = parseRig(b.loadFileAsString()); parsed.rig && parsed.rig->id == rigId)
                {
                    auto rig = b.getSiblingFile(b.getFileName().upToLastOccurrenceOf(".bak.", false, false));
                    knownPaths_[rigId] = rig;
                    return rig;
                }
    return juce::File();
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

    auto oldFile = fileFor(rig.id);
    // The path may come from a stale cache: never overwrite/delete a file that now holds another rig.
    if (!notForeign(oldFile, rig.id))
        oldFile = juce::File();
    const bool oldIsCorrupt = oldFile != juce::File() && oldFile.existsAsFile()
                              && !parseRig(oldFile.loadFileAsString()).rig;
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

    const bool ok = writeFileAtomically(target, serializeRig(rig), [&] {
        return !target.existsAsFile() || rotateBackups(target);
    });
    if (!ok)
        return juce::Result::fail("Cannot write " + target.getFullPathName());

    if (oldFile != juce::File() && oldFile != target && oldFile.existsAsFile())
    {
        moveBackups(oldFile, target);
        if (!oldIsCorrupt)
            pushBackup(target, oldFile); // the version saved under the old name/band stays restorable
        if (oldIsCorrupt)
        {
            // Never delete bytes we could not parse: park them beside the rig.
            juce::File parked;
            for (int n = 1;; ++n)
            {
                parked = oldFile.getSiblingFile(oldFile.getFileName() + ".corrupt" + (n == 1 ? juce::String() : "." + juce::String(n)));
                if (!parked.exists())
                    break;
            }
            oldFile.moveFileTo(parked);
        }
        else
            oldFile.deleteFile();
        pruneIfEmpty(oldFile.getParentDirectory());
    }
    knownPaths_[rig.id] = target;
    return juce::Result::ok();
}

juce::Result RigStore::remove(const juce::String& rigId)
{
    auto f = fileFor(rigId);
    if (f == juce::File())
        return juce::Result::fail("Rig not found: " + rigId);
    if (!notForeign(f, rigId))
        return juce::Result::fail("Refusing to delete " + f.getFullPathName() + ": it now holds a different rig");
    if (f.existsAsFile() && !trash_(f))
        return juce::Result::fail("Cannot move " + f.getFullPathName() + " to the trash");
    for (int i = 1; i <= maxBackups; ++i)
        if (auto b = backupFile(f, i); b.existsAsFile())
            trash_(b);
    knownPaths_.erase(rigId);
    pruneIfEmpty(f.getParentDirectory());
    return juce::Result::ok();
}

void RigStore::setTrashFunction(std::function<bool(const juce::File&)> fn) { trash_ = std::move(fn); }

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
