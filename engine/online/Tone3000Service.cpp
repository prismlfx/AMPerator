#include "Tone3000Service.h"

#include <juce_events/juce_events.h>

#include "Engine.h"

namespace amp::online
{
namespace
{
class NullHttpTransport final : public HttpTransport
{
public:
    HttpResponse request(const HttpRequest&) override
    {
        HttpResponse r;
        r.error = "network transport not available";
        return r;
    }
};

juce::var object(std::initializer_list<std::pair<const char*, juce::var>> props)
{
    auto* o = new juce::DynamicObject();
    for (auto& [k, v] : props)
        o->setProperty(k, v);
    return juce::var(o);
}

AuthConfig authConfig(const juce::String& clientId)
{
    AuthConfig c;
    c.clientId = clientId;
    c.redirectUri = Tone3000Service::redirectPrefix;
    c.storeKey = clientId; // Keychain account = the client id
    return c;
}
} // namespace

std::unique_ptr<HttpTransport> makeNullHttpTransport() { return std::make_unique<NullHttpTransport>(); }

juce::var Tone3000Status::toVar() const
{
    return object({ { "configured", configured },
                    { "connected", connected },
                    { "busy", busy },
                    { "remembered", remembered },
                    { "user", user ? online::toVar(*user) : juce::var() } });
}

Tone3000Service::Tone3000Service(Deps d)
    : deps(std::move(d)),
      store(deps.store),
      auth(authConfig(deps.clientId), deps.http, store),
      client(auth, deps.http, AuthConfig {}.baseUrl),
      providerIndex(deps.dataDir.getChildFile("tone3000").getChildFile("index.json")),
      downloads(client, deps.dataDir.getChildFile("tone3000").getChildFile(".partial"),
                deps.libraryRoot.getChildFile("Art"))
{
    jassert(deps.clientId.isNotEmpty());
    DownloadManager::cleanPartials(deps.dataDir.getChildFile("tone3000").getChildFile(".partial"));
    providerIndex.load();

    // Tokens gone for good (refresh refused) on any thread. disconnect() emits its own status for the sign-out it
    // causes, so the synchronous callback from its auth.clear() is skipped.
    auth.onSignedOut = [this, aliveFlag = alive]
    {
        if (juce::MessageManager::existsAndIsCurrentThread() && clearing)
            return;
        juce::MessageManager::callAsync([this, aliveFlag]
                                        {
                                            if (! aliveFlag->load())
                                                return;
                                            user.reset();
                                            emitStatus();
                                        });
    };

    if (auth.isConnected())
        loadUser();
}

Tone3000Service::~Tone3000Service()
{
    alive->store(false); // results still on their way are dropped
    downloads.cancelAll();
    pool.removeAllJobs(true, -1); // jobs use the units below; requests time out on their own
}

template <typename Work, typename Done>
void Tone3000Service::inBackground(Work work, Done done)
{
    pool.addJob([aliveFlag = alive, runWork = std::move(work), onDone = std::move(done)]() mutable
                {
                    if (! aliveFlag->load())
                        return;
                    auto deliver = [aliveFlag, finish = std::move(onDone), value = runWork()]() mutable
                    {
                        if (aliveFlag->load())
                            finish(std::move(value));
                    };
                    juce::MessageManager::callAsync(std::move(deliver));
                });
}

Tone3000Status Tone3000Service::status() const
{
    Tone3000Status s;
    s.configured = true;
    s.connected = auth.isConnected();
    s.busy = busy;
    s.remembered = store.remembered();
    if (s.connected)
        s.user = user;
    return s;
}

void Tone3000Service::emitStatus()
{
    if (emit)
        emit("tone3000Status", status().toVar());
}

void Tone3000Service::connect(std::function<void(CommandResult)> reply)
{
    if (busy)
        return reply(CommandResult::failure("busy"));
    if (! openAuthWindow)
        return reply(CommandResult::failure("not available"));

    busy = true;
    emitStatus();
    const auto start = auth.begin();
    auto called = std::make_shared<bool>(false);
    // The app calls done on the message thread; only its first call counts.
    openAuthWindow(start.url, redirectPrefix, [this, aliveFlag = alive, called, reply](const juce::String& query)
                   {
                       if (*called || ! aliveFlag->load())
                           return;
                       *called = true;
                       finishConnect(query, reply);
                   });
}

void Tone3000Service::finishConnect(const juce::String& query, std::function<void(CommandResult)> reply)
{
    auto finish = [this, reply](CommandResult r)
    {
        busy = false;
        emitStatus();
        reply(std::move(r));
    };

    if (query.isEmpty()) // the user closed the window
        return finish(CommandResult::success(object({ { "canceled", true } })));

    inBackground([this, query] { return auth.complete(query); },
                 [this, finish, reply](CallbackResult r)
                 {
                     if (! r.stateOk)
                     {
                         const juce::String message = "sign-in response didn't match the request";
                         if (emit)
                             emit("error", object({ { "message", message }, { "context", "tone3000" } }));
                         return finish(CommandResult::failure(message));
                     }
                     if (r.error.isNotEmpty())
                         return finish(CommandResult::failure(r.error));
                     if (r.signedIn)
                         return signedIn(reply);
                     if (r.canceled)
                         return finish(CommandResult::success(object({ { "canceled", true } })));
                     finish(CommandResult::failure("sign-in did not return an authorization code"));
                 });
}

void Tone3000Service::signedIn(std::function<void(CommandResult)> reply)
{
    inBackground([this] { return client.user(); },
                 [this, reply](Result<UserRef> r)
                 {
                     const bool connected = auth.isConnected(); // a disconnect may have happened meanwhile
                     user = connected ? r.value : std::nullopt;   // a failed fetch still leaves us connected
                     busy = false;
                     emitStatus();
                     reply(CommandResult::success(object({ { "connected", connected },
                                                           { "user", user ? online::toVar(*user) : juce::var() } })));
                 });
}

void Tone3000Service::loadUser()
{
    inBackground([this] { return client.user(); },
                 [this](Result<UserRef> r)
                 {
                     if (! r.value || ! auth.isConnected())
                         return;
                     user = r.value;
                     emitStatus();
                 });
}

void Tone3000Service::disconnect()
{
    downloads.cancelAll();
    clearing = true;
    auth.clear();
    clearing = false;
    user.reset();
    if (clearCookies)
        clearCookies();
    emitStatus();
}
} // namespace amp::online
