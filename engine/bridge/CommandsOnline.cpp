// Engine command handlers: TONE3000 status, connect / disconnect, browse, lists and tone detail
// (online/Tone3000Service).

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
} // namespace amp
