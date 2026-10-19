#pragma once

#include "model/RigDocument.h"

#include <optional>
#include <vector>

namespace amp::graph
{
/** Node ids from Input to Output; empty if the doc is not a single Input->...->Output chain. */
std::vector<juce::String> linearOrder(const RigDocument&);
bool isLinearChain(const RigDocument&);

/** Inserts `node` at `chainIndex` among the non-I/O nodes (clamped) and rewires.
    Assigns a fresh uuid if the id is empty or already used. Returns the id used,
    or an empty string (doc unchanged) if the doc is not a linear chain. */
juce::String insertNode(RigDocument&, NodeDoc node, int chainIndex);
/** False (doc unchanged) for unknown ids, Input/Output nodes, or non-linear docs. */
bool moveNode(RigDocument&, const juce::String& id, int newChainIndex);
/** Rewires neighbors and prunes mappings to `id`. False (doc unchanged) as moveNode. */
bool removeNode(RigDocument&, const juce::String& id);

/** Kahn's algorithm; nullopt on a cycle. */
std::optional<std::vector<juce::String>> topoOrder(const RigDocument&);
} // namespace amp::graph
