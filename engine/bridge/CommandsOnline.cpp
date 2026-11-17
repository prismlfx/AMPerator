// Engine command handlers: TONE3000 status, connect / disconnect, browse, lists, tone detail, downloads and fetching a
// rig's missing TONE3000 files (online/Tone3000Service), plus the rig side of a finished download.

#include "Engine.h"

#include "bridge/Bridge.h"
#include "online/Tone3000Service.h"

namespace amp
{
CommandResult Engine::tone3000Status()
{
    return CommandResult::success(tone3000 != nullptr ? tone3000->status().toVar() : online::Tone3000Status().toVar());
}

CommandResult Engine::tone3000Connect()
{
    if (tone3000 == nullptr)
        return CommandResult::failure("not configured");
    tone3000->openAuthWindow = onOpenAuthWindow; // the app's hook as it is now (null = "not available")
    tone3000->connect(bridge().deferReply());
    return CommandResult::deferred();
}

CommandResult Engine::tone3000Browse(const juce::String& format, const juce::String& nodeId,
                                     const juce::String& loadTone)
{
    if (tone3000 == nullptr)
        return CommandResult::failure("not configured");
    tone3000->openAuthWindow = onOpenAuthWindow;
    tone3000->browse({ format, nodeId, loadTone }, bridge().deferReply());
    return CommandResult::deferred();
}

CommandResult Engine::tone3000List(const juce::String& kind, int page)
{
    if (tone3000 == nullptr)
        return CommandResult::failure("not configured");
    tone3000->list(kind, page, bridge().deferReply());
    return CommandResult::deferred();
}

CommandResult Engine::tone3000Tone(const juce::String& id)
{
    if (tone3000 == nullptr)
        return CommandResult::failure("not configured");
    tone3000->tone(id, bridge().deferReply());
    return CommandResult::deferred();
}

CommandResult Engine::tone3000Disconnect()
{
    if (tone3000 == nullptr)
        return CommandResult::failure("not configured");
    tone3000->clearCookies = onClearAuthCookies;
    tone3000->disconnect();
    return CommandResult::success();
}

// ---- Downloads ----

CommandResult Engine::tone3000Download(const juce::String& modelId, const juce::String& toneId,
                                       const juce::String& nodeId, const juce::var& addNode)
{
    if (tone3000 == nullptr)
        return CommandResult::failure("not configured");
    if (nodeId.isNotEmpty())
    {
        const auto* node = findDocNode(nodeId);
        if (node == nullptr)
            return CommandResult::failure("unknown node: " + nodeId);
        if (node->type != NodeType::NamAmp && node->type != NodeType::CabIR)
            return CommandResult::failure("node " + nodeId + " takes no asset");
    }
    online::DownloadRequest request;
    request.modelId = modelId;
    request.toneId = toneId;
    request.nodeId = nodeId;
    request.addNode = addNode;
    if (live && (nodeId.isNotEmpty() || ! addNode.isVoid()))
        request.rigId = live->doc.id;
    juce::String error;
    const auto jobId = tone3000->download(std::move(request), error);
    if (error.isNotEmpty())
        return CommandResult::failure(error);
    return CommandResult::success(json::object({ { "jobId", jobId } }));
}

CommandResult Engine::tone3000Fetch(const juce::StringArray& nodeIds)
{
    if (tone3000 == nullptr)
        return CommandResult::failure("not configured");
    if (! tone3000->status().connected)
        return CommandResult::failure("not connected");
    return startFetch(nodeIds);
}

CommandResult Engine::tone3000Cancel(const juce::String& jobId)
{
    if (tone3000 == nullptr)
        return CommandResult::failure("not configured");
    tone3000->cancel(jobId);
    return CommandResult::success(json::object());
}

CommandResult Engine::startFetch(const juce::StringArray& nodeIds)
{
    if (! live)
        return CommandResult::failure("no rig loaded");
    const auto& fetchable = live->report.fetchable;
    for (const auto& id : nodeIds)
        if (! fetchable.contains(id))
            return CommandResult::failure("node " + id + " has no TONE3000 file to fetch");
    auto targets = nodeIds.isEmpty() ? fetchable : nodeIds;
    targets.removeDuplicates(false);

    juce::Array<juce::var> jobs;
    for (const auto& id : targets)
    {
        const auto* node = findDocNode(id);
        jassert(node != nullptr && node->asset && node->asset->source); // markFetchable checked
        online::DownloadRequest request;
        request.modelId = node->asset->source->id;
        request.nodeId = id;
        request.expectedSha = node->asset->sha256;
        request.rigId = live->doc.id;
        juce::String error;
        const auto jobId = tone3000->download(std::move(request), error);
        if (error.isNotEmpty())
            return CommandResult::failure(error);
        jobs.add(jobId);
    }
    return CommandResult::success(json::object({ { "jobs", jobs } }));
}

void Engine::markFetchable(LiveRig& rig) const
{
    rig.report.fetchable.clear();
    for (const auto& n : rig.doc.nodes)
        if (rig.report.placeholderNodeIds.contains(n.id) && n.asset && n.asset->source
            && n.asset->source->provider == "tone3000" && n.asset->source->id.isNotEmpty())
            rig.report.fetchable.add(n.id);
}

void Engine::autoFetch()
{
    // The user's standing choice, and only with a session already open: never opens a window.
    if (tone3000 != nullptr && settings.tone3000AutoFetch && live && ! live->report.fetchable.isEmpty()
        && tone3000->status().connected)
        startFetch({});
}

CommandResult Engine::placeDownload(const juce::File& file, const juce::String& sha256,
                                    const online::DownloadRequest& request)
{
    if (! live || live->doc.id != request.rigId)
        return CommandResult::success(); // another rig is loaded now: the file stays in the library only

    const auto provenance = tone3000->index().bySha(sha256);
    const juce::String format = provenance ? provenance->format : file.hasFileExtension("nam") ? "nam" : "ir";
    const auto fits = format == "nam" ? NodeType::NamAmp : NodeType::CabIR;
    auto mismatch = [&](NodeType type)
    {
        return CommandResult::failure("that tone is a " + format + " file and cannot be used on a "
                                      + json::nodeTypeName(type) + " node");
    };

    if (request.nodeId.isNotEmpty())
    {
        const auto* node = findDocNode(request.nodeId);
        if (node == nullptr)
            return CommandResult::success(); // removed meanwhile
        if (node->type != fits)
            return mismatch(node->type);
        return setAsset(request.nodeId, file.getFullPathName());
    }
    const auto type = json::parseNodeType(request.addNode["type"].toString());
    if (! type)
        return CommandResult::failure("unknown node type: " + request.addNode["type"].toString());
    if (*type != fits)
        return mismatch(*type);
    return addNode(*type, static_cast<int>(request.addNode["index"]), {}, file.getFullPathName());
}
} // namespace amp
