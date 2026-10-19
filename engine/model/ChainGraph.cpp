#include "model/ChainGraph.h"

#include <algorithm>
#include <map>
#include <set>

namespace amp::graph
{
namespace
{
const NodeDoc* findNode(const RigDocument& doc, const juce::String& id)
{
    for (const auto& n : doc.nodes)
        if (n.id == id)
            return &n;
    return nullptr;
}

void setChain(RigDocument& doc, const std::vector<juce::String>& order)
{
    doc.connections.clear();
    for (size_t i = 0; i + 1 < order.size(); ++i)
        doc.connections.emplace_back(order[i], order[i + 1]);
}

int clampIndex(int index, size_t count)
{
    return std::clamp(index, 0, static_cast<int>(count));
}
} // namespace

std::vector<juce::String> linearOrder(const RigDocument& doc)
{
    const NodeDoc* in = nullptr;
    const NodeDoc* out = nullptr;
    std::set<juce::String> ids;
    for (const auto& n : doc.nodes)
    {
        if (! ids.insert(n.id).second)
            return {};
        if (n.type == NodeType::Input)
        {
            if (in != nullptr) return {};
            in = &n;
        }
        else if (n.type == NodeType::Output)
        {
            if (out != nullptr) return {};
            out = &n;
        }
    }
    if (in == nullptr || out == nullptr || doc.connections.size() + 1 != doc.nodes.size())
        return {};

    std::map<juce::String, juce::String> next;
    for (const auto& [from, to] : doc.connections)
    {
        if (! ids.count(from) || ! ids.count(to) || from == out->id || to == in->id)
            return {};
        if (! next.emplace(from, to).second)
            return {};
    }

    std::vector<juce::String> order { in->id };
    while (order.back() != out->id)
    {
        auto it = next.find(order.back());
        if (it == next.end() || order.size() > doc.nodes.size())
            return {};
        order.push_back(it->second);
    }
    return order.size() == doc.nodes.size() ? order : std::vector<juce::String>{};
}

bool isLinearChain(const RigDocument& doc)
{
    return ! linearOrder(doc).empty();
}

juce::String insertNode(RigDocument& doc, NodeDoc node, int chainIndex)
{
    auto order = linearOrder(doc);
    if (order.empty())
        return {};

    if (node.id.isEmpty() || findNode(doc, node.id) != nullptr)
        node.id = juce::Uuid().toString();

    const auto id = node.id;
    order.insert(order.begin() + 1 + clampIndex(chainIndex, order.size() - 2), id);
    doc.nodes.push_back(std::move(node));
    setChain(doc, order);
    return id;
}

bool moveNode(RigDocument& doc, const juce::String& id, int newChainIndex)
{
    const auto* node = findNode(doc, id);
    auto order = linearOrder(doc);
    if (node == nullptr || order.empty() || node->type == NodeType::Input || node->type == NodeType::Output)
        return false;

    order.erase(std::find(order.begin(), order.end(), id));
    order.insert(order.begin() + 1 + clampIndex(newChainIndex, order.size() - 2), id);
    setChain(doc, order);
    return true;
}

bool removeNode(RigDocument& doc, const juce::String& id)
{
    const auto* node = findNode(doc, id);
    auto order = linearOrder(doc);
    if (node == nullptr || order.empty() || node->type == NodeType::Input || node->type == NodeType::Output)
        return false;

    order.erase(std::find(order.begin(), order.end(), id));
    std::erase_if(doc.nodes, [&](const NodeDoc& n) { return n.id == id; });
    std::erase_if(doc.mappings, [&](const Mapping& m) { return m.node == id; });
    setChain(doc, order);
    return true;
}

std::optional<std::vector<juce::String>> topoOrder(const RigDocument& doc)
{
    std::map<juce::String, int> indegree;
    std::map<juce::String, std::vector<juce::String>> out;
    for (const auto& n : doc.nodes)
        indegree[n.id] = 0;
    for (const auto& [from, to] : doc.connections)
    {
        if (! indegree.count(from) || ! indegree.count(to))
            continue;
        out[from].push_back(to);
        ++indegree[to];
    }

    std::vector<juce::String> ready, result;
    for (const auto& n : doc.nodes)   // document order keeps the result deterministic
        if (indegree[n.id] == 0)
            ready.push_back(n.id);

    for (size_t i = 0; i < ready.size(); ++i)
    {
        result.push_back(ready[i]);
        for (const auto& to : out[ready[i]])
            if (--indegree[to] == 0)
                ready.push_back(to);
    }
    if (result.size() != indegree.size())
        return std::nullopt;
    return result;
}
} // namespace amp::graph
