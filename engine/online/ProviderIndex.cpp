#include "ProviderIndex.h"

#include "storage/AtomicFile.h"

namespace amp::online
{
namespace
{
juce::var object(std::initializer_list<std::pair<const char*, juce::var>> props)
{
    auto* o = new juce::DynamicObject();
    for (auto& p : props)
        o->setProperty(p.first, p.second);
    return juce::var(o);
}

juce::String str(const juce::var& o, const char* name)
{
    const auto v = o.getProperty(name, {});
    return v.isString() ? v.toString() : juce::String();
}

juce::var recordVar(const Provenance& p)
{
    auto creator = object({ { "id", p.creator.id },
                            { "username", p.creator.username },
                            { "displayName", p.creator.displayName },
                            { "avatarUrl", p.creator.avatarUrl },
                            { "url", p.creator.url } });
    return object({ { "provider", p.provider },
                    { "modelId", p.modelId },
                    { "toneId", p.toneId },
                    { "title", p.title },
                    { "modelName", p.modelName },
                    { "size", p.size },
                    { "architecture", p.architecture },
                    { "format", p.format },
                    { "gear", p.gear },
                    { "license", p.license },
                    { "creator", creator },
                    { "toneUrl", p.toneUrl },
                    { "image", p.image },
                    { "downloadedAt", p.downloadedAt } });
}

Provenance readRecord(const juce::var& v)
{
    Provenance p;
    p.provider = str(v, "provider");
    if (p.provider.isEmpty())
        p.provider = "tone3000";
    p.modelId = str(v, "modelId");
    p.toneId = str(v, "toneId");
    p.title = str(v, "title");
    p.modelName = str(v, "modelName");
    p.size = str(v, "size");
    p.architecture = str(v, "architecture");
    p.format = str(v, "format");
    p.gear = str(v, "gear");
    p.license = str(v, "license");
    p.toneUrl = str(v, "toneUrl");
    p.image = str(v, "image");
    p.downloadedAt = str(v, "downloadedAt");
    const auto c = v.getProperty("creator", {});
    p.creator.id = str(c, "id");
    p.creator.username = str(c, "username");
    p.creator.displayName = str(c, "displayName");
    p.creator.avatarUrl = str(c, "avatarUrl");
    p.creator.url = str(c, "url");
    return p;
}
} // namespace

juce::var toVar(const Provenance& p)
{
    return object({ { "provider", p.provider },
                    { "id", p.modelId },
                    { "toneId", p.toneId },
                    { "title", p.title },
                    { "creator", toVar(p.creator) },
                    { "license", p.license },
                    { "url", p.toneUrl },
                    { "image", p.image } });
}

ProviderIndex::ProviderIndex(juce::File indexFile) : file(std::move(indexFile)) {}

void ProviderIndex::load()
{
    entries.clear();
    if (! file.existsAsFile())
        return;

    const auto root = juce::JSON::parse(file.loadFileAsString());
    if (auto* o = root.getDynamicObject())
    {
        for (auto& prop : o->getProperties())
            if (prop.value.getDynamicObject() != nullptr)
                entries[prop.name.toString().toLowerCase()] = readRecord(prop.value);
        return;
    }

    auto kept = file.getSiblingFile(file.getFileName() + ".corrupt");
    for (int n = 2; kept.exists(); ++n)
        kept = file.getSiblingFile(file.getFileName() + ".corrupt." + juce::String(n));
    file.moveFileTo(kept);
}

bool ProviderIndex::save() const
{
    if (! file.getParentDirectory().createDirectory())
        return false;
    auto* o = new juce::DynamicObject();
    for (auto& [sha, p] : entries)
        o->setProperty(juce::Identifier(sha), recordVar(p));
    return writeFileAtomically(file, juce::JSON::toString(juce::var(o)));
}

void ProviderIndex::put(const juce::String& sha256, Provenance p) { entries[sha256.toLowerCase()] = std::move(p); }

std::optional<Provenance> ProviderIndex::bySha(const juce::String& sha256) const
{
    const auto it = entries.find(sha256.toLowerCase());
    if (it == entries.end())
        return std::nullopt;
    return it->second;
}

std::optional<juce::String> ProviderIndex::shaForModel(const juce::String& modelId) const
{
    if (modelId.isEmpty())
        return std::nullopt;
    for (auto& [sha, p] : entries)
        if (p.modelId == modelId)
            return sha;
    return std::nullopt;
}

size_t ProviderIndex::size() const { return entries.size(); }
} // namespace amp::online
