#pragma once

#include <juce_core/juce_core.h>

#include <optional>
#include <utility>
#include <vector>

namespace amp
{
enum class NodeType { Input, Output, Gate, EQ, NamAmp, CabIR, Plugin, Placeholder };

struct AssetSource { juce::String provider, id; };
struct AssetRef    { juce::String kind /*"nam"|"ir"*/, file, sha256; std::optional<AssetSource> source; };
struct PluginRef   { juce::String format, uid, name, vendor; };

struct NodeDoc
{
    juce::String id;
    NodeType type = NodeType::Placeholder;
    bool bypass = false;
    juce::NamedValueSet params;
    std::optional<AssetRef> asset;
    std::optional<PluginRef> plugin;
    juce::MemoryBlock state;
    juce::String artArchetype;
    // Known types: unknown keys of the node object, preserved on save.
    // Placeholder: the complete original node object, written back unchanged.
    juce::var extra;
};

struct Mapping { juce::String control, action, node; };

struct RigDocument
{
    juce::String id, name, band;
    juce::StringArray tags;
    std::vector<NodeDoc> nodes;
    std::vector<std::pair<juce::String, juce::String>> connections;
    std::vector<Mapping> mappings;
};

struct ParseResult
{
    std::optional<RigDocument> rig;
    juce::String error;
};

/** Never throws. On failure `rig` is empty and `error` describes why. */
ParseResult parseRig(const juce::String& json);
/** A Placeholder node with no original object in `extra` is skipped (not written). */
juce::String serializeRig(const RigDocument&);

/** in -> Gate -> NamAmp(empty) -> CabIR(empty) -> out, with a fresh uuid. */
RigDocument makeDefaultRig(const juce::String& name, const juce::String& band);
} // namespace amp
