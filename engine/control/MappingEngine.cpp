#include "control/MappingEngine.h"

#include <algorithm>

namespace amp
{
namespace
{
const juce::String kToggleBypass { "toggleBypass" };
}

MappingEngine::MappingEngine(BypassFn fn) : bypassFn(std::move(fn)) {}

void MappingEngine::setMappings(std::vector<Mapping> rig, std::vector<Mapping> global)
{
    rigMappings = std::move(rig);
    globalMappings = std::move(global);
}

void MappingEngine::setValidNodes(juce::StringArray ids)
{
    validNodes = std::move(ids);
    if (learnNode && !validNodes.contains(*learnNode))
    {
        learnNode.reset(); // armed node was deleted: disarm
        if (onLearnCancelled)
            onLearnCancelled();
    }
}

void MappingEngine::beginLearn(const juce::String& nodeId)
{
    if (validNodes.contains(nodeId))
        learnNode = nodeId;
}

void MappingEngine::cancelLearn() { learnNode.reset(); }

std::vector<Mapping> MappingEngine::effectiveMappings() const
{
    std::vector<Mapping> out;
    for (auto& m : rigMappings)
        if (validNodes.contains(m.node))
            out.push_back(m);
    for (auto& m : globalMappings)
    {
        const bool overridden = std::any_of(rigMappings.begin(), rigMappings.end(),
                                            [&](const Mapping& r) { return r.control == m.control; });
        if (!overridden && validNodes.contains(m.node))
            out.push_back(m);
    }
    return out;
}

void MappingEngine::handle(const ControlEvent& e)
{
    if (!e.pressed)
        return;

    if (learnNode)
    {
        Mapping m { e.control, kToggleBypass, *learnNode };
        learnNode.reset();
        std::erase_if(rigMappings, [&](const Mapping& r) { return r.control == e.control; });
        rigMappings.push_back(m);
        if (onLearned)
            onLearned(m);
        return;
    }

    // Rig mappings shadow global ones for the same control, even if the rig one is stale/unknown.
    const Mapping* hit = nullptr;
    for (auto& m : rigMappings)
        if (m.control == e.control) { hit = &m; break; }
    if (hit == nullptr)
        for (auto& m : globalMappings)
            if (m.control == e.control) { hit = &m; break; }

    if (hit == nullptr || hit->action != kToggleBypass || !validNodes.contains(hit->node))
        return;

    // Copy before calling out: callbacks may re-enter setMappings and invalidate `hit`.
    const juce::String node = hit->node;
    if (auto state = bypassFn(node, std::nullopt))
        if (onBypassChanged)
            onBypassChanged(node, *state);
}
} // namespace amp
