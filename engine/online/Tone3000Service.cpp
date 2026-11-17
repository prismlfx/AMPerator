#include "Tone3000Service.h"

#include <ctime>
#include <stdexcept>

#include <juce_events/juce_events.h>

#include "Engine.h"
#include "Placement.h"
#include "assets/AssetIndex.h"

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

// Now as ISO 8601 UTC with seconds, e.g. "2026-10-20T18:02:11Z" (Provenance::downloadedAt).
juce::String nowIso8601Utc()
{
    const auto t = std::time(nullptr);
    std::tm utc {};
#if JUCE_WINDOWS
    gmtime_s(&utc, &t);
#else
    gmtime_r(&t, &utc);
#endif
    char text[32] = {};
    std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return text;
}

bool hasNodeTarget(const DownloadRequest& r) { return r.nodeId.isNotEmpty() || ! r.addNode.isVoid(); }

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
    downloads.onEvent = [this](const JobEvent& e) { onJobEvent(e); }; // message thread, never after destruction

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
    cancelDownloads();
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

void Tone3000Service::emitStatusIfChanged(const juce::String& before)
{
    if (juce::JSON::toString(status().toVar(), true) != before)
        emitStatus();
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
                             undoAbandonedSignIn(); // the exchange finished after the disconnect
                         return finish(f, CommandResult::success(canceledResult())); // already replied: no-op
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
                             undoAbandonedSignIn();
                         return finish(f, CommandResult::success(canceledResult())); // already replied: no-op
                     }
                     if (! r.stateOk)
                         return stateMismatch(f);
                     if (r.signedIn && ! connected) // the session had ended while the picker was open
                     {
                         connected = true;
                         emitStatus();
                         loadUser();
                     }
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

void Tone3000Service::undoAbandonedSignIn()
{
    if (connected) // a newer sign-in finished first and owns the session now
        return;
    const auto before = juce::JSON::toString(status().toVar(), true);
    signOutQuietly();
    emitStatusIfChanged(before);
}

void Tone3000Service::disconnect()
{
    JUCE_ASSERT_MESSAGE_THREAD
    if (flow != nullptr) // replied now; whatever its window or exchange reports later changes nothing
    {
        auto abandoned = flow;
        abandoned->abandoned = true;
        flow = nullptr;
        finish(abandoned, CommandResult::success(canceledResult()));
    }
    busy = false;
    cancelDownloads();
    signOutQuietly();
    if (clearCookies)
        clearCookies();
    emitStatus();
}

// ---- Downloads ----

juce::String Tone3000Service::download(DownloadRequest request, juce::String& error)
{
    JUCE_ASSERT_MESSAGE_THREAD
    error = {};
    if (! connected)
    {
        error = "not connected";
        return {};
    }

    // Already in the library: the newest indexed file of this model, found by hash.
    if (const auto sha = providerIndex.shaForModel(request.modelId))
    {
        const auto p = providerIndex.bySha(*sha);
        deps.assets.rescan();
        const auto found = deps.assets.resolve(AssetRef { p->format == "nam" ? "nam" : "ir", {}, *sha, std::nullopt });
        if (found.how == ResolveKind::ByHash)
        {
            request.jobId = juce::Uuid().toString();
            instantJobs[request.jobId] = false;
            JobEvent e;
            e.jobId = request.jobId;
            e.request = request;
            e.tone.id = p->toneId;
            juce::MessageManager::callAsync(
                [this, aliveFlag = alive, e, file = found.file, sha = *sha]() mutable
                {
                    if (! aliveFlag->load())
                        return;
                    const auto it = instantJobs.find(e.jobId);
                    const bool canceled = it == instantJobs.end() || it->second;
                    instantJobs.erase(e.jobId);
                    emitJob(e); // queued
                    if (canceled)
                    {
                        e.state = JobState::Failed;
                        e.error = "canceled";
                        return emitJob(e);
                    }
                    juce::String placeError;
                    if (hasNodeTarget(e.request) && placeInRig)
                        if (auto r = placeInRig(file, sha, e.request); ! r.ok)
                            placeError = r.error;
                    e.state = JobState::Done;
                    e.progress = 1;
                    emitJob(e, file, sha);
                    if (placeError.isNotEmpty() && emit)
                        emit("error", object({ { "message", placeError }, { "context", "tone3000" } }));
                });
            return request.jobId;
        }
    }
    return downloads.start(std::move(request));
}

void Tone3000Service::cancel(const juce::String& jobId)
{
    JUCE_ASSERT_MESSAGE_THREAD
    if (auto it = instantJobs.find(jobId); it != instantJobs.end())
        it->second = true;
    else
        downloads.cancel(jobId);
}

void Tone3000Service::cancelDownloads()
{
    for (auto& job : instantJobs)
        job.second = true;
    downloads.cancelAll();
}

void Tone3000Service::emitJob(const JobEvent& e, const juce::File& placed, const juce::String& sha)
{
    if (! emit)
        return;
    auto v = online::toVar(e);
    if (e.state == JobState::Done)
    {
        auto* o = v.getDynamicObject();
        o->setProperty("file", placed.getFullPathName());
        o->setProperty("sha256", sha);
        if (e.request.expectedSha.isNotEmpty())
            o->setProperty("updated", ! e.request.expectedSha.equalsIgnoreCase(sha));
    }
    emit("tone3000Download", v);
}

void Tone3000Service::onJobEvent(const JobEvent& e)
{
    if (e.state == JobState::Done)
        return finishDownload(e);
    emitJob(e);
}

// Message thread, one at a time: placeFile's check-then-move needs that.
void Tone3000Service::finishDownload(const JobEvent& e)
{
    Provenance p;
    p.modelId = e.model.id.isNotEmpty() ? e.model.id : e.request.modelId;
    p.toneId = e.tone.id;
    p.title = e.tone.title;
    p.modelName = e.model.name;
    p.size = e.model.size;
    p.architecture = e.model.architecture;
    p.format = e.tone.format;
    p.gear = e.tone.gear;
    p.license = e.tone.license;
    p.creator = e.tone.creator;
    p.toneUrl = e.tone.url;
    const auto artDir = deps.libraryRoot.getChildFile("Art");
    if (e.image.existsAsFile() && e.image.isAChildOf(artDir))
        p.image = e.image.getRelativePathFrom(artDir).replaceCharacter('\\', '/');
    p.downloadedAt = nowIso8601Utc();

    juce::String error;
    const auto placed = placeFile(e.file, placementFor(deps.libraryRoot, p), e.sha256, error);
    if (placed == juce::File())
    {
        e.file.deleteFile();
        JobEvent failed;
        failed.jobId = e.jobId;
        failed.request = e.request;
        failed.tone.id = e.tone.id;
        failed.state = JobState::Failed;
        failed.error = error;
        return emitJob(failed);
    }

    providerIndex.put(e.sha256, std::move(p));
    providerIndex.save();
    deps.assets.rescan();

    juce::String placeError;
    if (hasNodeTarget(e.request) && placeInRig)
        if (auto r = placeInRig(placed, e.sha256, e.request); ! r.ok)
            placeError = r.error;
    emitJob(e, placed, e.sha256);
    if (placeError.isNotEmpty() && emit)
        emit("error", object({ { "message", placeError }, { "context", "tone3000" } }));
}
} // namespace amp::online
