#include "Tone3000Service.h"

#include <stdexcept>

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

juce::var canceledResult() { return object({ { "canceled", true } }); }

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

    // Tokens gone for good (refresh refused) on any thread. signOutQuietly() emits its own status for the sign-out it
    // causes, so the synchronous callback from its auth.clear() is skipped.
    auth.onSignedOut = [this, aliveFlag = alive]
    {
        if (juce::MessageManager::existsAndIsCurrentThread() && clearing)
            return;
        juce::MessageManager::callAsync([this, aliveFlag]
                                        {
                                            if (! aliveFlag->load())
                                                return;
                                            connected = auth.isConnected(); // a newer sign-in may have won
                                            if (! connected)
                                                user.reset();
                                            emitStatus();
                                        });
    };

    connected = auth.isConnected();
    if (connected)
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
    JUCE_ASSERT_MESSAGE_THREAD
    Tone3000Status s;
    s.configured = true;
    s.connected = connected;
    s.busy = busy;
    s.remembered = store.remembered();
    if (connected)
        s.user = user;
    return s;
}

void Tone3000Service::emitStatus()
{
    if (emit)
        emit("tone3000Status", status().toVar());
}

// ---- Window flows (connect, browse) ----

Tone3000Service::FlowPtr Tone3000Service::beginFlow(Reply reply)
{
    if (busy)
    {
        reply(CommandResult::failure("busy"));
        return nullptr;
    }
    if (! openAuthWindow)
    {
        reply(CommandResult::failure("not available"));
        return nullptr;
    }
    flow = std::make_shared<Flow>();
    flow->reply = std::move(reply);
    busy = true;
    emitStatus();
    return flow;
}

void Tone3000Service::finish(const FlowPtr& f, CommandResult r)
{
    if (f->replied)
        return;
    f->replied = true;
    if (! f->abandoned) // an abandoned flow no longer owns busy (disconnect cleared it)
    {
        flow = nullptr;
        busy = false;
        emitStatus();
    }
    f->reply(std::move(r));
}

void Tone3000Service::openWindow(const FlowPtr& f, const juce::String& url,
                                 std::function<void(const juce::String&)> onQuery)
{
    auto called = std::make_shared<bool>(false);
    auto done = [this, aliveFlag = alive, f, called, onQuery](const juce::String& query)
    {
        JUCE_ASSERT_MESSAGE_THREAD
        if (*called || ! aliveFlag->load())
            return;
        *called = true;
        if (f->abandoned) // disconnected meanwhile: whatever the window says is dropped
            return finish(f, CommandResult::success(canceledResult()));
        onQuery(query);
    };
    try
    {
        if (! openAuthWindow)
            throw std::runtime_error("no auth window");
        openAuthWindow(url, redirectPrefix, done);
    }
    catch (...)
    {
        *called = true; // a done() the hook may still hold is spent
        finish(f, CommandResult::failure("not available"));
    }
}

void Tone3000Service::stateMismatch(const FlowPtr& f)
{
    const juce::String message = "sign-in response didn't match the request";
    if (emit)
        emit("error", object({ { "message", message }, { "context", "tone3000" } }));
    finish(f, CommandResult::failure(message));
}

void Tone3000Service::signIn(const FlowPtr& f, const juce::String& query, std::function<void()> signedIn)
{
    if (query.isEmpty()) // the user closed the window
        return finish(f, CommandResult::success(canceledResult()));

    inBackground([this, query] { return auth.complete(query); },
                 [this, f, signedIn](CallbackResult r)
                 {
                     if (f->abandoned)
                     {
                         if (r.signedIn)
                             signOutQuietly(); // the exchange finished after the disconnect: undo it
                         return finish(f, CommandResult::success(canceledResult()));
                     }
                     if (! r.stateOk)
                         return stateMismatch(f);
                     if (r.error.isNotEmpty())
                         return finish(f, CommandResult::failure(r.error));
                     if (r.signedIn)
                     {
                         connected = true;
                         return signedIn();
                     }
                     if (r.canceled)
                         return finish(f, CommandResult::success(canceledResult()));
                     finish(f, CommandResult::failure("sign-in did not return an authorization code"));
                 });
}

void Tone3000Service::connect(Reply reply)
{
    JUCE_ASSERT_MESSAGE_THREAD
    auto f = beginFlow(std::move(reply));
    if (f == nullptr)
        return;
    openWindow(f, auth.begin().url,
               [this, f](const juce::String& query)
               {
                   signIn(f, query, [this, f]
                          {
                              inBackground([this] { return client.user(); },
                                           [this, f](Result<UserRef> r)
                                           {
                                               if (f->abandoned)
                                                   return finish(f, CommandResult::success(canceledResult()));
                                               user = connected ? r.value : std::nullopt; // a failed fetch: no user
                                               const auto u = user ? online::toVar(*user) : juce::var();
                                               finish(f, CommandResult::success(
                                                             object({ { "connected", connected }, { "user", u } })));
                                           });
                          });
               });
}

void Tone3000Service::browse(const BrowseRequest& request, Reply reply)
{
    JUCE_ASSERT_MESSAGE_THREAD
    auto f = beginFlow(std::move(reply));
    if (f == nullptr)
        return;
    if (connected)
        return openPicker(f, request);

    // Sign in first in a window of its own (the plain authorize URL), then open the picker.
    openWindow(f, auth.begin().url,
               [this, f, request](const juce::String& query)
               {
                   signIn(f, query, [this, f, request]
                          {
                              emitStatus();
                              loadUser();
                              openPicker(f, request);
                          });
               });
}

void Tone3000Service::openPicker(const FlowPtr& f, const BrowseRequest& request)
{
    juce::StringPairArray params;
    if (request.loadTone.isNotEmpty())
    {
        params.set("prompt", "load_tone");
        params.set("tone_id", request.loadTone);
    }
    else
    {
        params.set("prompt", "select_tone");
        params.set("preview", "true");
        params.set("menubar", "true");
        params.set("architecture", "2");
    }
    if (request.format.isNotEmpty())
        params.set("format", request.format);

    openWindow(f, auth.begin(params).url,
               [this, f, request](const juce::String& query) { pickerDone(f, request, query); });
}

void Tone3000Service::pickerDone(const FlowPtr& f, const BrowseRequest& request, const juce::String& query)
{
    if (query.isEmpty())
        return finish(f, CommandResult::success(canceledResult()));

    // Always through complete(): it checks the state, and exchanges a code the picker may send along.
    inBackground([this, query] { return auth.complete(query); },
                 [this, f, request](CallbackResult r)
                 {
                     if (f->abandoned)
                     {
                         if (r.signedIn)
                             signOutQuietly();
                         return finish(f, CommandResult::success(canceledResult()));
                     }
                     if (! r.stateOk)
                         return stateMismatch(f);
                     if (r.signedIn)
                         connected = true;
                     if (r.canceled || (r.toneId.isEmpty() && r.error.isEmpty()))
                         return finish(f, CommandResult::success(canceledResult()));
                     if (r.toneId.isEmpty())
                         return finish(f, CommandResult::failure(r.error));
                     auto result = object({ { "toneId", r.toneId } });
                     if (request.nodeId.isNotEmpty())
                         result.getDynamicObject()->setProperty("nodeId", request.nodeId);
                     finish(f, CommandResult::success(result));
                 });
}

// ---- Lists and tone detail ----

template <typename T>
void Tone3000Service::replyWith(Result<T> r, Reply reply, std::function<juce::var(const T&)> shape)
{
    if (r.error)
    {
        if (r.error.kind == ClientError::Kind::NotConnected && connected)
        {
            connected = false; // tokens vanished without a sign-out callback
            user.reset();
            emitStatus();
        }
        return reply(CommandResult::failure(userMessage(r.error)));
    }
    reply(CommandResult::success(shape(*r.value)));
}

void Tone3000Service::list(const juce::String& kind, int page, Reply reply)
{
    JUCE_ASSERT_MESSAGE_THREAD
    if (! connected)
        return reply(CommandResult::failure("not connected"));
    inBackground([this, kind, page] { return client.list(kind, page); },
                 [this, reply](Result<Page> r)
                 {
                     replyWith<Page>(std::move(r), reply,
                                     [](const Page& p)
                                     {
                                         juce::Array<juce::var> tones;
                                         for (const auto& t : p.tones)
                                             tones.add(online::toVar(t));
                                         return object({ { "tones", tones },
                                                         { "page", p.page },
                                                         { "totalPages", p.totalPages } });
                                     });
                 });
}

void Tone3000Service::tone(const juce::String& id, Reply reply)
{
    JUCE_ASSERT_MESSAGE_THREAD
    if (! connected)
        return reply(CommandResult::failure("not connected"));
    inBackground([this, id] { return client.tone(id); },
                 [this, reply](Result<Tone> r)
                 { replyWith<Tone>(std::move(r), reply, [](const Tone& t) { return online::toVar(t); }); });
}

// ---- Session ----

void Tone3000Service::loadUser()
{
    inBackground([this] { return client.user(); },
                 [this](Result<UserRef> r)
                 {
                     if (! r.value || ! connected)
                         return;
                     user = r.value;
                     emitStatus();
                 });
}

void Tone3000Service::signOutQuietly()
{
    clearing = true;
    auth.clear();
    clearing = false;
    connected = false;
    user.reset();
}

void Tone3000Service::disconnect()
{
    JUCE_ASSERT_MESSAGE_THREAD
    if (flow != nullptr) // its window may still report: that callback then replies {canceled: true}
    {
        flow->abandoned = true;
        flow = nullptr;
    }
    busy = false;
    downloads.cancelAll();
    signOutQuietly();
    if (clearCookies)
        clearCookies();
    emitStatus();
}
} // namespace amp::online
