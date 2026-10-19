#include "RigDocument.h"

#include <cmath>

namespace amp
{
namespace
{
constexpr const char* kFormat = "amperator.rig";
constexpr int kVersion = 1;

struct TypeName { NodeType type; const char* name; };
constexpr TypeName kTypeNames[] = {
    { NodeType::Input, "Input" },   { NodeType::Output, "Output" }, { NodeType::Gate, "Gate" },
    { NodeType::EQ, "EQ" },         { NodeType::NamAmp, "NamAmp" }, { NodeType::CabIR, "CabIR" },
    { NodeType::Plugin, "Plugin" }
};

NodeType typeFromString(const juce::String& s)
{
    for (const auto& t : kTypeNames)
        if (s == t.name)
            return t.type;
    return NodeType::Placeholder;
}

const char* typeToString(NodeType t)
{
    for (const auto& e : kTypeNames)
        if (e.type == t)
            return e.name;
    return "";
}

bool isKnownNodeKey(const juce::Identifier& k)
{
    static const juce::StringArray keys { "id", "type", "bypass", "params", "asset", "plugin", "state", "art" };
    return keys.contains(k.toString());
}

juce::var makeObject() { return juce::var(new juce::DynamicObject()); }

AssetRef parseAsset(const juce::var& v)
{
    AssetRef a;
    a.kind = v["kind"].toString();
    a.file = v["file"].toString();
    a.sha256 = v["sha256"].toString();
    if (v["source"].isObject())
        a.source = AssetSource { v["source"]["provider"].toString(), v["source"]["id"].toString() };
    return a;
}

PluginRef parsePlugin(const juce::var& v)
{
    return { v["format"].toString(), v["uid"].toString(), v["name"].toString(), v["vendor"].toString() };
}

bool parseNode(const juce::var& v, NodeDoc& n, juce::String& error)
{
    auto* obj = v.getDynamicObject();
    if (obj == nullptr || ! v["id"].isString() || ! v["type"].isString())
    {
        error = "node must be an object with string \"id\" and \"type\"";
        return false;
    }

    n.id = v["id"].toString();
    n.type = typeFromString(v["type"].toString());
    n.bypass = static_cast<bool>(v["bypass"]);
    n.artArchetype = v["art"]["archetype"].toString();

    if (auto* p = v["params"].getDynamicObject())
        for (const auto& prop : p->getProperties())
            if (prop.value.isInt() || prop.value.isInt64() || prop.value.isDouble() || prop.value.isBool())
                n.params.set(prop.name, prop.value);

    if (v["asset"].isObject())
        n.asset = parseAsset(v["asset"]);
    if (v["plugin"].isObject())
        n.plugin = parsePlugin(v["plugin"]);

    if (v["state"].isString())
    {
        juce::MemoryOutputStream mb;
        if (! juce::Base64::convertFromBase64(mb, v["state"].toString()))
        {
            error = "node \"" + n.id + "\": invalid base64 in \"state\"";
            return false;
        }
        n.state = mb.getMemoryBlock();
    }

    if (n.type == NodeType::Placeholder)
    {
        n.extra = v.clone();
    }
    else
    {
        auto extra = makeObject();
        for (const auto& prop : obj->getProperties())
            if (! isKnownNodeKey(prop.name))
                extra.getDynamicObject()->setProperty(prop.name, prop.value);
        n.extra = extra;
    }
    return true;
}

juce::String toBase64(const juce::MemoryBlock& mb)
{
    juce::MemoryOutputStream out;
    juce::Base64::convertToBase64(out, mb.getData(), mb.getSize());
    return out.toString();
}

juce::var paramsToVar(const juce::NamedValueSet& params)
{
    auto v = makeObject();
    for (int i = 0; i < params.size(); ++i)
        v.getDynamicObject()->setProperty(params.getName(i), params.getValueAt(i));
    return v;
}

// Returns void for a Placeholder with no original object (nothing sensible to write).
juce::var nodeToVar(const NodeDoc& n)
{
    if (n.type == NodeType::Placeholder)
    {
        if (! n.extra.isObject())
            return {};
        auto out = n.extra.clone();
        auto* o = out.getDynamicObject();
        o->setProperty("id", n.id);
        o->setProperty("bypass", n.bypass);
        if (n.params.size() > 0)
            o->setProperty("params", paramsToVar(n.params));
        return out;
    }

    auto out = makeObject();
    auto* o = out.getDynamicObject();

    // Unknown keys first, so known keys always win.
    if (auto* e = n.extra.getDynamicObject())
        for (const auto& prop : e->getProperties())
            o->setProperty(prop.name, prop.value);

    o->setProperty("id", n.id);
    o->setProperty("type", typeToString(n.type));
    o->setProperty("bypass", n.bypass);

    if (n.params.size() > 0)
        o->setProperty("params", paramsToVar(n.params));

    if (n.asset)
    {
        auto a = makeObject();
        a.getDynamicObject()->setProperty("kind", n.asset->kind);
        a.getDynamicObject()->setProperty("file", n.asset->file);
        a.getDynamicObject()->setProperty("sha256", n.asset->sha256);
        if (n.asset->source)
        {
            auto s = makeObject();
            s.getDynamicObject()->setProperty("provider", n.asset->source->provider);
            s.getDynamicObject()->setProperty("id", n.asset->source->id);
            a.getDynamicObject()->setProperty("source", s);
        }
        o->setProperty("asset", a);
    }

    if (n.plugin)
    {
        auto p = makeObject();
        p.getDynamicObject()->setProperty("format", n.plugin->format);
        p.getDynamicObject()->setProperty("uid", n.plugin->uid);
        p.getDynamicObject()->setProperty("name", n.plugin->name);
        p.getDynamicObject()->setProperty("vendor", n.plugin->vendor);
        o->setProperty("plugin", p);
    }

    if (n.state.getSize() > 0)
        o->setProperty("state", toBase64(n.state));

    if (n.artArchetype.isNotEmpty())
    {
        auto art = makeObject();
        art.getDynamicObject()->setProperty("archetype", n.artArchetype);
        o->setProperty("art", art);
    }
    return out;
}

ParseResult fail(const juce::String& msg)
{
    ParseResult r;
    r.error = msg;
    return r;
}

ParseResult parseRigImpl(const juce::String& json)
{
    juce::var root;
    if (juce::JSON::parse(json, root).failed() || ! root.isObject())
        return fail("malformed JSON: expected a rig object");

    if (root["format"].toString() != kFormat)
        return fail("unsupported \"format\" (expected \"" + juce::String(kFormat) + "\")");

    const auto& ver = root["version"];
    const double verNum = static_cast<double>(ver);
    if (! (ver.isInt() || ver.isInt64() || ver.isDouble()) || verNum != std::floor(verNum) || verNum < 1)
        return fail("missing or invalid \"version\"");
    if (verNum > kVersion)
        return fail("rig \"version\" " + ver.toString() + " is newer than supported version " + juce::String(kVersion));

    RigDocument rig;
    rig.id = root["id"].toString();
    rig.name = root["name"].toString();
    rig.band = root["band"].toString();

    if (auto* tags = root["tags"].getArray())
        for (const auto& t : *tags)
            rig.tags.add(t.toString());

    if (auto* nodes = root["nodes"].getArray())
        for (const auto& nv : *nodes)
        {
            NodeDoc n;
            juce::String err;
            if (! parseNode(nv, n, err))
                return fail(err);
            rig.nodes.push_back(std::move(n));
        }

    if (auto* conns = root["connections"].getArray())
        for (const auto& c : *conns)
        {
            auto* pair = c.getArray();
            if (pair == nullptr || pair->size() != 2)
                return fail("each entry in \"connections\" must be a [from, to] pair");
            rig.connections.emplace_back((*pair)[0].toString(), (*pair)[1].toString());
        }

    if (auto* maps = root["mappings"].getArray())
        for (const auto& m : *maps)
            rig.mappings.push_back({ m["control"].toString(), m["action"].toString(), m["node"].toString() });

    ParseResult r;
    r.rig = std::move(rig);
    return r;
}
} // namespace

ParseResult parseRig(const juce::String& json)
{
    try
    {
        return parseRigImpl(json);
    }
    catch (const std::exception& e)
    {
        return fail(juce::String("parse failed: ") + e.what());
    }
    catch (...)
    {
        return fail("parse failed");
    }
}

juce::String serializeRig(const RigDocument& rig)
{
    auto root = makeObject();
    auto* o = root.getDynamicObject();
    o->setProperty("format", kFormat);
    o->setProperty("version", kVersion);
    o->setProperty("id", rig.id);
    o->setProperty("name", rig.name);
    o->setProperty("band", rig.band);

    juce::Array<juce::var> tags;
    for (const auto& t : rig.tags)
        tags.add(t);
    o->setProperty("tags", tags);

    juce::Array<juce::var> nodes;
    for (const auto& n : rig.nodes)
        if (auto v = nodeToVar(n); ! v.isVoid())
            nodes.add(v);
    o->setProperty("nodes", nodes);

    juce::Array<juce::var> conns;
    for (const auto& [from, to] : rig.connections)
        conns.add(juce::Array<juce::var> { from, to });
    o->setProperty("connections", conns);

    juce::Array<juce::var> maps;
    for (const auto& m : rig.mappings)
    {
        auto mv = makeObject();
        mv.getDynamicObject()->setProperty("control", m.control);
        mv.getDynamicObject()->setProperty("action", m.action);
        mv.getDynamicObject()->setProperty("node", m.node);
        maps.add(mv);
    }
    o->setProperty("mappings", maps);

    return juce::JSON::toString(root, false);
}

RigDocument makeDefaultRig(const juce::String& name, const juce::String& band)
{
    RigDocument rig;
    rig.id = juce::Uuid().toString();
    rig.name = name;
    rig.band = band;

    const NodeType types[] = { NodeType::Input, NodeType::Gate, NodeType::NamAmp, NodeType::CabIR, NodeType::Output };
    for (auto t : types)
    {
        NodeDoc n;
        n.id = juce::Uuid().toString();
        n.type = t;
        rig.nodes.push_back(std::move(n));
    }
    for (size_t i = 0; i + 1 < rig.nodes.size(); ++i)
        rig.connections.emplace_back(rig.nodes[i].id, rig.nodes[i + 1].id);
    return rig;
}
} // namespace amp
