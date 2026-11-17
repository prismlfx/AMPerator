// Engine command handlers: TONE3000 status, connect and disconnect (online/Tone3000Service).

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

CommandResult Engine::tone3000Disconnect()
{
    if (tone3000 == nullptr)
        return CommandResult::failure("not configured");
    tone3000->clearCookies = onClearAuthCookies;
    tone3000->disconnect();
    return CommandResult::success();
}
} // namespace amp
