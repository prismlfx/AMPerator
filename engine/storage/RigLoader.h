#pragma once

#include <memory>
#include <optional>

#include "assets/AssetIndex.h"
#include "engine/ProcessingPlan.h"
#include "model/RigDocument.h"
#include "plugins/PluginHost.h"

namespace amp
{
struct LoadReport
{
    juce::StringArray missingAssets, missingPlugins; // asset file names / plugin names that could not be loaded
    juce::StringArray placeholderNodeIds;            // nodes running as bypassed placeholders
    juce::StringArray resolvedByFilename;            // asset files matched by name, not hash (warn the user)
};

struct LiveRig
{
    RigDocument doc;
    std::shared_ptr<ProcessingPlan> plan;
    LoadReport report;
};

// Turns a RigDocument into a prepared ProcessingPlan and captures live state back into a document.
// The plan holds the nodes between Input and Output in chain order (Input/Output are not processor nodes).
// Gate/EQ are built-ins; NamAmp/CabIR load their asset (resolved through the AssetIndex) or run with none
// when the node has no asset; Plugin nodes are created through the PluginHost. Anything that cannot be
// loaded (missing/unloadable asset, missing plugin, unknown node type) becomes a passthrough placeholder
// with bypass = true, listed in the report; its NodeDoc stays untouched in LiveRig::doc.
class RigLoader
{
public:
    RigLoader(AssetIndex&, PluginHost&);

    // Off the audio thread; call on the message thread if the rig contains plugins (PluginHost requirement),
    // otherwise any thread. Every node is fresh (never shared with another plan) and prepared with `spec`.
    // nullopt + `error` if the doc is cyclic or not a single Input -> ... -> Output chain.
    std::optional<LiveRig> build(const RigDocument&, const ProcessSpec&, juce::String& error);

    // live.doc with each loaded node's current bypass (from the plan), params (getParams(), merged over the
    // doc's) and, for plugins, state (getState()). Placeholder nodes are returned unchanged. An asset that was
    // matched by file name gets the matched file's sha256, so the next load resolves it by hash.
    // Call on the thread that owns the AssetIndex (message thread).
    RigDocument capture(const LiveRig&) const;

private:
    AssetIndex& assets;
    PluginHost& plugins;
};
} // namespace amp
