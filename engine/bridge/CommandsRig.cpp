// Engine command handlers: rig library (load/save/step/delete/backups/favorites), chain edits, live edits,
// mapping learn and node art. Split out of Engine.cpp to keep the facade's wiring readable.

#include "Engine.h"

#include "bridge/Bridge.h"
#include "model/ChainGraph.h"
#include "storage/AtomicFile.h"

namespace amp
{
namespace
{
juce::String assetKindFor(NodeType t)
{
    return t == NodeType::NamAmp ? "nam" : t == NodeType::CabIR ? "ir" : juce::String();
}

NodeDoc* findIn(RigDocument& doc, const juce::String& id)
{
    for (auto& n : doc.nodes)
        if (n.id == id)
            return &n;
    return nullptr;
}
} // namespace

// ---- Library ----

CommandResult Engine::listRigs()
{
    juce::Array<juce::var> rigs;
    for (auto& e : store.list())
        rigs.add(json::object({ { "id", e.id }, { "name", e.name }, { "band", e.band },
                                { "favorite", settings.favoriteRigIds.contains(e.id) } }));
    return CommandResult::success(json::object({ { "rigs", rigs }, { "bands", json::strings(store.bands()) } }));
}

CommandResult Engine::loadRig(const juce::String& id)
{
    auto parsed = store.load(id);
    if (! parsed.rig)
        return CommandResult::failure(parsed.error);
    if (auto error = installRig(*parsed.rig, true, false); error.isNotEmpty())
        return CommandResult::failure(error);
    settings.lastRigId = id;
    settings.noteRecent(id);
    saveSettings();
    return CommandResult::success(json::object({ { "id", id } }));
}

CommandResult Engine::newRig(const juce::String& name, const juce::String& band)
{
    const auto doc = makeDefaultRig(name.trim().isNotEmpty() ? name.trim() : juce::String("New Rig"),
                                    band.trim().isNotEmpty() ? band.trim() : juce::String("My Tones"));
    if (auto error = installRig(doc, false, false); error.isNotEmpty())
        return CommandResult::failure(error);
    return CommandResult::success(json::object({ { "id", doc.id } }));
}

CommandResult Engine::saveRig(const juce::String& name, const juce::String& band)
{
    if (! live)
        return CommandResult::failure("no rig loaded");
    auto doc = currentDoc();
    if (name.trim().isNotEmpty())
        doc.name = name.trim();
    if (band.trim().isNotEmpty())
        doc.band = band.trim();
    if (const auto r = store.save(doc); r.failed())
        return CommandResult::failure(r.getErrorMessage());
    live->doc = doc;
    saved = true;
    settings.lastRigId = doc.id;
    settings.noteRecent(doc.id);
    saveSettings();
    dirty = false;
    bridge().emit("rigDirty", json::object({ { "dirty", false } }));
    return CommandResult::success(json::object({ { "id", doc.id }, { "name", doc.name }, { "band", doc.band } }));
}

CommandResult Engine::deleteRig(const juce::String& id)
{
    const auto before = store.list();
    int index = -1;
    for (int i = 0; i < (int) before.size(); ++i)
        if (before[(size_t) i].id == id)
            index = i;

    if (const auto r = store.remove(id); r.failed())
        return CommandResult::failure(r.getErrorMessage());

    settings.favoriteRigIds.removeString(id);
    settings.recentRigIds.removeString(id);
    if (settings.lastRigId == id)
        settings.lastRigId = {};

    if (live && saved && live->doc.id == id)
    {
        // Load the rig that took its place in list order, else a fresh default.
        const auto after = store.list();
        bool loaded = false;
        if (! after.empty())
            loaded = loadRig(after[(size_t) juce::jlimit(0, (int) after.size() - 1, index)].id).ok;
        if (! loaded)
            installRig(makeDefaultRig("New Rig", "My Tones"), false, false);
    }
    saveSettings();
    return CommandResult::success();
}

CommandResult Engine::stepRig(int direction)
{
    const auto rigs = store.list();
    if (rigs.empty())
        return CommandResult::failure("no saved rigs");
    const int n = (int) rigs.size();
    int current = -1; // an unsaved rig sits "before the first": next -> first, prev -> last
    if (live && saved)
        for (int i = 0; i < n; ++i)
            if (rigs[(size_t) i].id == live->doc.id)
                current = i;
    const int target = current < 0 ? (direction > 0 ? 0 : n - 1) : ((current + direction) % n + n) % n;
    return loadRig(rigs[(size_t) target].id);
}

CommandResult Engine::restoreBackup(const juce::String& id)
{
    const auto backups = store.backups(id);
    if (backups.empty())
        return CommandResult::failure("no backup for rig " + id);
    const auto& newest = backups.front();
    const auto text = newest.loadFileAsString();
    if (! parseRig(text).rig)
        return CommandResult::failure("the newest backup cannot be read either: " + newest.getFileName());
    const auto rigFile = newest.getSiblingFile(newest.getFileName().upToLastOccurrenceOf(".bak.", false, false));
    if (rigFile.existsAsFile())
    {
        // Never destroy the current file: keep it next to the rig, as <file>.corrupt (same naming as RigStore) when
        // unreadable, else as <file>.replaced.
        const auto suffix = parseRig(rigFile.loadFileAsString()).rig ? juce::String(".replaced") : juce::String(".corrupt");
        auto parked = rigFile.getSiblingFile(rigFile.getFileName() + suffix);
        for (int n = 2; parked.exists(); ++n)
            parked = rigFile.getSiblingFile(rigFile.getFileName() + suffix + "." + juce::String(n));
        if (! rigFile.copyFileTo(parked))
            return CommandResult::failure("could not keep a copy of the current rig file");
    }
    if (! writeFileAtomically(rigFile, text))
        return CommandResult::failure("could not restore " + rigFile.getFullPathName());
    return loadRig(id);
}

CommandResult Engine::toggleFavorite(const juce::String& id)
{
    const bool favorite = ! settings.favoriteRigIds.contains(id);
    if (favorite)
        settings.favoriteRigIds.add(id);
    else
        settings.favoriteRigIds.removeString(id);
    saveSettings();
    return CommandResult::success(json::object({ { "id", id }, { "favorite", favorite } }));
}

// ---- Chain edits (rebuild + swap) ----

CommandResult Engine::addNode(NodeType type, int index, const juce::var& plugin, const juce::String& assetFile)
{
    if (type == NodeType::Input || type == NodeType::Output || type == NodeType::Placeholder)
        return CommandResult::failure("cannot add a node of type " + json::nodeTypeName(type));

    NodeDoc node;
    node.type = type;
    if (type == NodeType::Plugin)
    {
        const auto uid = plugin.isString() ? plugin.toString() : plugin["uid"].toString();
        if (uid.isEmpty())
            return CommandResult::failure("addNode of type Plugin needs \"plugin\" (a plugin from listPlugins)");
        for (auto& d : plugins->available())
            if (PluginHost::refFor(d).uid == uid)
                node.plugin = PluginHost::refFor(d);
        if (! node.plugin)
            return CommandResult::failure("unknown plugin: " + uid);
    }
    if (assetFile.isNotEmpty())
    {
        const auto kind = assetKindFor(type);
        if (kind.isEmpty())
            return CommandResult::failure("node type " + json::nodeTypeName(type) + " takes no asset");
        node.asset = assetRefFor(kind, assetFile);
        if (! node.asset)
            return CommandResult::failure("not in the asset library: " + assetFile);
    }

    juce::String id;
    auto r = structuralEdit([&](RigDocument& doc)
    {
        id = graph::insertNode(doc, node, index);
        return id.isEmpty() ? juce::String("the rig is not a single chain") : juce::String();
    });
    if (r.ok)
        r.result = json::object({ { "id", id } });
    return r;
}

CommandResult Engine::moveNode(const juce::String& id, int index)
{
    return structuralEdit([&](RigDocument& doc)
                          { return graph::moveNode(doc, id, index) ? juce::String() : "cannot move node " + id; });
}

CommandResult Engine::removeNode(const juce::String& id)
{
    return structuralEdit([&](RigDocument& doc)
                          { return graph::removeNode(doc, id) ? juce::String() : "cannot remove node " + id; });
}

CommandResult Engine::setAsset(const juce::String& id, const juce::String& file)
{
    const auto* node = findDocNode(id);
    if (node == nullptr)
        return CommandResult::failure("unknown node: " + id);
    const auto kind = assetKindFor(node->type);
    if (kind.isEmpty())
        return CommandResult::failure("node " + id + " takes no asset");
    std::optional<AssetRef> ref; // empty file clears the asset
    if (file.isNotEmpty())
    {
        ref = assetRefFor(kind, file);
        if (! ref)
            return CommandResult::failure("not in the asset library: " + file);
    }
    return structuralEdit([&](RigDocument& doc)
    {
        if (auto* n = findIn(doc, id))
            n->asset = ref;
        return juce::String();
    });
}

// ---- Live edits (no rebuild) ----

CommandResult Engine::setParam(const juce::String& id, const juce::String& name, float value)
{
    auto* pn = live ? live->plan->find(id) : nullptr;
    if (pn == nullptr)
        return CommandResult::failure("unknown node: " + id);
    if (isPlaceholder(id))
        return CommandResult::failure("node " + id + " is not loaded (placeholder)");
    if (! pn->node->getParams().contains(name))
        return CommandResult::failure("node " + id + " has no parameter \"" + name + "\"");
    pn->node->setParam(name, value);
    const auto actual = pn->node->getParams()[juce::Identifier(name)];
    bridge().emit("paramChanged", json::object({ { "id", id }, { "name", name }, { "value", actual } }));
    setDirty(true);
    return CommandResult::success(json::object({ { "value", actual } }));
}

CommandResult Engine::toggleBypass(const juce::String& id)
{
    const auto state = applyBypass(id, std::nullopt);
    if (! state)
        return CommandResult::failure("unknown or unloaded node: " + id);
    bridge().emit("bypassChanged", json::object({ { "id", id }, { "bypass", *state } }));
    setDirty(true);
    return CommandResult::success(json::object({ { "bypass", *state } }));
}

// ---- Mapping learn ----

CommandResult Engine::learnMapping(const juce::String& id)
{
    mapping.cancelLearn(); // a new request replaces any armed learn, also when it is rejected
    mapping.beginLearn(id);
    if (! mapping.isLearning())
        return CommandResult::failure("node cannot be mapped: " + id);
    return CommandResult::success();
}

CommandResult Engine::cancelLearn()
{
    mapping.cancelLearn();
    return CommandResult::success();
}

// ---- Node art ----

CommandResult Engine::setNodeArt(const juce::String& id, const std::optional<juce::String>& archetype,
                                 const std::optional<juce::String>& imageFile)
{
    auto* node = findDocNode(id);
    if (node == nullptr)
        return CommandResult::failure("unknown node: " + id);

    if (imageFile)
    {
        const juce::Identifier key(live->doc.id + "/" + id);
        auto* entries = userArt.getDynamicObject();
        if (imageFile->isEmpty())
            entries->removeProperty(key);
        else
        {
            if (! juce::File::isAbsolutePath(*imageFile) || ! juce::File(*imageFile).existsAsFile())
                return CommandResult::failure("image file not found: " + *imageFile);
            const juce::File src(*imageFile);
            // A generated ASCII name ([A-Za-z0-9._-] only): the web view's art/ URL then needs no encoding, so it
            // resolves the same on every platform (JUCE hands the resource provider a decoded path on macOS but the
            // raw percent-encoded one on Windows).
            const auto safe = [](const juce::String& text)
            {
                juce::String out;
                for (auto c : text)
                    out << ((c < 128 && juce::CharacterFunctions::isLetterOrDigit(c)) || c == '.' || c == '_' || c == '-'
                                ? juce::String::charToString(c) : juce::String("_"));
                return out;
            };
            const auto hash = juce::String::toHexString((src.getFullPathName() + key.toString()
                                                         + juce::String(juce::Time::currentTimeMillis())).hashCode64())
                                  .paddedLeft('0', 16).substring(0, 8);
            const auto ext = safe(src.getFileExtension().toLowerCase().trimCharactersAtStart("."));
            const auto dest = artDir().getNonexistentChildFile(safe(live->doc.id).substring(0, 40) + "-" + safe(id).substring(0, 40)
                                                                   + "-" + hash,
                                                               ext.isEmpty() ? juce::String() : "." + ext, false);
            if (! src.copyFileTo(dest))
                return CommandResult::failure("could not copy the image into " + artDir().getFullPathName());
            entries->setProperty(key, dest.getFileName());
        }
        if (! writeFileAtomically(artDir().getChildFile("user-art.json"), juce::JSON::toString(userArt)))
            return CommandResult::failure("could not write user-art.json");
    }
    if (archetype)
    {
        node->artArchetype = *archetype; // part of the rig
        setDirty(true);
    }
    return CommandResult::success(json::object({ { "id", id }, { "art", artFor(*node) } }));
}
} // namespace amp
