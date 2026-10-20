#include "storage/RigLoader.h"

#include "dsp/BuiltinNodes.h"
#include "dsp/CabIRNode.h"
#include "dsp/NamAmpNode.h"
#include "model/ChainGraph.h"

#include <map>

namespace amp
{
namespace
{
bool hasAsset(const NodeDoc& n)
{
    return n.asset.has_value() && (n.asset->file.isNotEmpty() || n.asset->sha256.isNotEmpty());
}

const NodeDoc* findNode(const RigDocument& doc, const juce::String& id)
{
    for (const auto& n : doc.nodes)
        if (n.id == id)
            return &n;
    return nullptr;
}

juce::String assetLabel(const AssetRef& ref) { return ref.file.isNotEmpty() ? ref.file : ref.sha256; }

juce::String pluginLabel(const NodeDoc& n)
{
    if (! n.plugin.has_value())
        return n.id;
    return n.plugin->name.isNotEmpty() ? n.plugin->name : n.plugin->uid;
}
} // namespace

RigLoader::RigLoader(AssetIndex& a, PluginHost& p) : assets(a), plugins(p) {}

std::optional<LiveRig> RigLoader::build(const RigDocument& doc, const ProcessSpec& spec, juce::String& error)
{
    if (! graph::topoOrder(doc).has_value())
    {
        error = "rig graph has a cycle or duplicate node ids";
        return std::nullopt;
    }
    const auto order = graph::linearOrder(doc);
    if (order.empty())
    {
        error = "rig is not a single Input -> ... -> Output chain";
        return std::nullopt;
    }

    LiveRig live;
    live.doc = doc;
    std::vector<std::shared_ptr<PlanNode>> planNodes;

    for (const auto& id : order)
    {
        const NodeDoc& n = *findNode(doc, id);
        if (n.type == NodeType::Input || n.type == NodeType::Output)
            continue;

        // Resolves the node's asset as `kind`; returns the file, or an empty File (reported) when missing.
        auto resolveAsset = [&](const juce::String& kind) -> juce::File
        {
            auto ref = *n.asset;
            ref.kind = kind;
            const auto r = assets.resolve(ref);
            if (r.how == ResolveKind::Missing)
                return {};
            if (r.how == ResolveKind::ByFilename)
                live.report.resolvedByFilename.add(r.file.getFullPathName());
            return r.file;
        };

        std::unique_ptr<ProcessorNode> node;
        switch (n.type)
        {
            case NodeType::Gate: node = makeGate(); break;
            case NodeType::EQ: node = makeEQ(); break;
            case NodeType::NamAmp:
            {
                auto nam = makeNamAmp();
                if (hasAsset(n))
                {
                    juce::String loadError;
                    const auto file = resolveAsset("nam");
                    auto model = file != juce::File() ? NamAmpNode::loadModel(file, loadError) : nullptr;
                    if (model == nullptr)
                    {
                        live.report.missingAssets.add(assetLabel(*n.asset));
                        break;
                    }
                    nam->setModel(std::move(model));
                }
                node = std::move(nam);
                break;
            }
            case NodeType::CabIR:
            {
                auto cab = makeCabIR();
                if (hasAsset(n))
                {
                    juce::String loadError;
                    const auto file = resolveAsset("ir");
                    if (file == juce::File() || ! cab->loadIR(file, loadError))
                    {
                        live.report.missingAssets.add(assetLabel(*n.asset));
                        break;
                    }
                }
                node = std::move(cab);
                break;
            }
            case NodeType::Plugin:
            {
                juce::String createError;
                if (n.plugin.has_value())
                    node = plugins.create(*n.plugin, spec, createError);
                if (node == nullptr)
                    live.report.missingPlugins.add(pluginLabel(n));
                else if (n.state.getSize() > 0)
                    node->setState(n.state);
                break;
            }
            case NodeType::Input:
            case NodeType::Output:
            case NodeType::Placeholder: break;
        }

        auto pn = std::make_shared<PlanNode>();
        pn->id = n.id;
        if (node == nullptr)
        {
            node = makePlaceholder();
            pn->bypass.store(true);
            live.report.placeholderNodeIds.add(n.id);
        }
        else
        {
            for (const auto& p : n.params)
                node->setParam(p.name.toString(), static_cast<float>(static_cast<double>(p.value)));
            pn->bypass.store(n.bypass);
        }
        node->prepare(spec);
        pn->node = std::move(node);
        planNodes.push_back(std::move(pn));
    }

    live.plan = std::make_shared<ProcessingPlan>(std::move(planNodes), spec);
    return live;
}

RigDocument RigLoader::capture(const LiveRig& live) const
{
    RigDocument doc = live.doc;
    for (auto& n : doc.nodes)
    {
        if (n.type == NodeType::Input || n.type == NodeType::Output || live.report.placeholderNodeIds.contains(n.id))
            continue;
        auto* pn = live.plan->find(n.id);
        if (pn == nullptr)
            continue;

        n.bypass = pn->bypass.load();
        for (const auto& p : pn->node->getParams())
            n.params.set(p.name, p.value);
        if (n.type == NodeType::Plugin)
            n.state = pn->node->getState();

        // A file matched by name (stale hash in the doc): store the matched file's hash so the next load
        // resolves by hash even if the file is renamed later.
        if ((n.type == NodeType::NamAmp || n.type == NodeType::CabIR) && hasAsset(n))
        {
            auto ref = *n.asset;
            ref.kind = n.type == NodeType::NamAmp ? "nam" : "ir";
            const auto r = assets.resolve(ref);
            if (r.how == ResolveKind::ByFilename)
            {
                juce::String sha;
                for (const auto& e : assets.list(ref.kind))
                    if (e.file == r.file)
                        sha = e.sha256;
                if (sha.isEmpty())
                    sha = AssetIndex::sha256Of(r.file);
                if (sha.isNotEmpty())
                    n.asset->sha256 = sha;
            }
        }
    }
    return doc;
}
} // namespace amp
