#pragma once

#include <juce_core/juce_core.h>

#include <functional>
#include <optional>
#include <vector>

#include "assets/AssetIndex.h"
#include "model/RigDocument.h"
#include "storage/RigLoader.h"
#include "storage/Settings.h"

namespace amp
{
class Engine;

/** Versioned JSON command/event bridge between the web UI and the Engine (see docs/bridge-protocol.md).
    Message thread only.
      in:    {"v":1,"cmd":"...","id":n,"args":{...}}
      reply: {"v":1,"reply":n,"ok":true,"result":{...}} or {"v":1,"reply":n,"ok":false,"error":"..."}
      event: {"v":1,"event":"...","data":{...}}
    Malformed input never throws: it is answered with ok:false. */
class Bridge
{
public:
    static constexpr int protocolVersion = 1;

    explicit Bridge(Engine&);

    void handle(const juce::var& message);
    std::function<void(const juce::var&)> send; // replies + events

    /** Sends {"v":1,"event":name,"data":data}. Before `send` is set, other events are dropped but `error` events are
        queued and delivered (in order) ahead of the next reply or event once `send` is set. */
    void emit(const juce::String& name, const juce::var& data);

private:
    void reply(const juce::var& id, bool ok, const juce::var& payload);
    void flushPending();

    std::vector<juce::var> pending; // error events emitted before `send` was set

    Engine& engine;
};

/** JSON shaping shared by the Bridge and the Engine's event payloads. */
namespace json
{
juce::var object(std::initializer_list<std::pair<const char*, juce::var>> props = {});
juce::var strings(const juce::StringArray&);
juce::var mappings(const std::vector<Mapping>&);
juce::var pluginRef(const PluginRef&);
juce::var report(const LoadReport&);
juce::var asset(const AssetEntry&);
/** Settings as stored (footswitchKeys keyed "0x04"). */
juce::var settings(const Settings&);
/** The rig as in an .amprig file, but with `nodes` listed in signal order and every node carrying
    `art` = artFor(node) ({archetype, image}). */
juce::var rig(const RigDocument&, const std::function<juce::var(const NodeDoc&)>& artFor);

juce::String nodeTypeName(NodeType);
std::optional<NodeType> parseNodeType(const juce::String&);
} // namespace json
} // namespace amp
