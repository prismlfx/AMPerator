#include "assets/AssetIndex.h"

#include <juce_cryptography/juce_cryptography.h>

#include <algorithm>

namespace amp
{
namespace
{
juce::String kindFor(const juce::File& f)
{
    if (f.hasFileExtension("nam"))
        return "nam";
    if (f.hasFileExtension("wav;aiff;aif"))
        return "ir";
    return {};
}

bool inHiddenDir(const juce::File& f, const juce::File& root)
{
    for (auto d = f.getParentDirectory(); d != root && d.getFullPathName().length() > root.getFullPathName().length();
         d = d.getParentDirectory())
        if (d.getFileName().startsWithChar('.'))
            return true;
    return false;
}

juce::String fileNameOnly(const juce::String& path)
{
    return path.fromLastOccurrenceOf("/", false, false).fromLastOccurrenceOf("\\", false, false);
}
} // namespace

void AssetIndex::setRoots(juce::Array<juce::File> r) { roots = std::move(r); }

juce::String AssetIndex::sha256Of(const juce::File& f)
{
    juce::FileInputStream in(f);
    if (! in.openedOk())
        return {};
    return juce::SHA256(in).toHexString().toLowerCase();
}

void AssetIndex::rescan()
{
    std::map<juce::String, AssetEntry> next;

    for (const auto& root : roots)
    {
        for (const auto& f : root.findChildFiles(juce::File::findFiles, true))
        {
            const auto kind = kindFor(f);
            if (kind.isEmpty() || f.isHidden() || f.getFileName().startsWithChar('.') || inHiddenDir(f, root))
                continue;

            const auto path = f.getFullPathName();
            if (next.count(path) != 0)
                continue; // overlapping roots

            const auto size = f.getSize();
            const auto mod = f.getLastModificationTime().toMilliseconds();

            auto it = cache.find(path);
            if (it != cache.end() && it->second.size == size && it->second.modTime == mod)
            {
                next[path] = it->second;
                continue;
            }

            auto sha = sha256Of(f);
            ++hashes;
            if (sha.isEmpty())
                continue;
            next[path] = AssetEntry { f, kind, sha, size, mod };
        }
    }

    cache = std::move(next);
    entries.clear();
    for (const auto& kv : cache) // std::map is ordered by path
        entries.push_back(kv.second);
}

Resolved AssetIndex::resolve(const AssetRef& ref) const
{
    if (ref.sha256.isNotEmpty())
    {
        const auto want = ref.sha256.toLowerCase();
        for (const auto& e : entries)
            if (e.kind == ref.kind && e.sha256 == want)
                return { ResolveKind::ByHash, e.file };
    }

    const auto name = fileNameOnly(ref.file);
    if (name.isNotEmpty())
        for (const auto& e : entries)
            if (e.kind == ref.kind && e.file.getFileName().equalsIgnoreCase(name))
                return { ResolveKind::ByFilename, e.file };

    return {};
}

std::vector<AssetEntry> AssetIndex::list(const juce::String& kind) const
{
    std::vector<AssetEntry> out;
    for (const auto& e : entries)
        if (e.kind == kind)
            out.push_back(e);
    return out;
}
} // namespace amp
