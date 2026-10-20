#pragma once

#include <juce_core/juce_core.h>

#include <functional>
#include <optional>
#include <vector>

#include "control/ControlEvent.h"
#include "model/RigDocument.h"

namespace amp
{
/** Routes ControlEvents to actions (v1: "toggleBypass") with MIDI-learn.
    Rig mappings override global ones for the same control. Message thread only. */
class MappingEngine
{
public:
    /** Deviation from the plan's `bool` return: nullopt = node not found (no callback fires).
        `set` = nullopt toggles; otherwise forces that state. Returns the new bypass state. */
    using BypassFn = std::function<std::optional<bool>(const juce::String& nodeId, std::optional<bool> set)>;

    explicit MappingEngine(BypassFn);

    void setMappings(std::vector<Mapping> rig, std::vector<Mapping> global);
    void setValidNodes(juce::StringArray ids);

    /** Only pressed=true acts. While learning, the next press of any control is captured instead. */
    void handle(const ControlEvent&);

    /** Arms learn for `nodeId`; ignored if the node is not in the valid set. */
    void beginLearn(const juce::String& nodeId);
    void cancelLearn();
    bool isLearning() const { return learnNode.has_value(); }

    std::function<void(const Mapping&)> onLearned; // caller writes the mapping into the rig doc
    std::function<void(const juce::String& node, bool bypass)> onBypassChanged;

    /** Rig mappings (insertion order) then non-overridden global ones, minus those whose node is invalid. */
    std::vector<Mapping> effectiveMappings() const;

private:
    BypassFn bypassFn;
    std::vector<Mapping> rigMappings, globalMappings;
    juce::StringArray validNodes;
    std::optional<juce::String> learnNode;
};
} // namespace amp
