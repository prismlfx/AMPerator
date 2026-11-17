#pragma once

// The TONE3000 facade the Engine calls (tone-library spec section 5.1): connection status, connect / disconnect, the
// picker (browse), bounded lists and tone detail.
// Owns the auth, client, download manager, provenance index and a small pool for blocking calls; turns their results
// into bridge events. Message thread only at its public surface; worker results come back through callAsync, guarded
// so nothing reaches a destroyed service.

#include <functional>
#include <memory>
#include <optional>

#include <juce_core/juce_core.h>

#include "DownloadManager.h"
#include "HttpTransport.h"
#include "ProviderIndex.h"
#include "SecureStore.h"
#include "Tone3000Auth.h"
#include "Tone3000Client.h"
#include "Tone3000Types.h"

namespace amp
{
class AssetIndex;
struct CommandResult;
} // namespace amp

namespace amp::online
{
// Bridge shape Tone3000Status (spec section 7.4). remembered = false when the platform store refused writes and the
// tokens live only in memory for this session (spec section 11).
struct Tone3000Status
{
    bool configured = false, connected = false, busy = false, remembered = true;
    std::optional<UserRef> user;

    // {configured, connected, busy, remembered, user: UserRef | null}
    juce::var toVar() const;
};

// Until the real transport exists: every request fails with "network transport not available".
std::unique_ptr<HttpTransport> makeNullHttpTransport();

class Tone3000Service
{
public:
    struct Deps
    {
        juce::String clientId; // non-empty
        HttpTransport& http;
        SecureStore& store; // the platform store; wrapped in a FallbackSecureStore
        juce::File dataDir, libraryRoot;
        AssetIndex& assets;
    };

    // What tone3000Browse asked for: format "nam" | "ir" | "" (any); nodeId is echoed in the reply; a non-empty
    // loadTone opens prompt=load_tone for that tone instead of the select_tone picker.
    struct BrowseRequest
    {
        juce::String format, nodeId, loadTone;
    };

    using Reply = std::function<void(CommandResult)>;

    static constexpr const char* redirectPrefix = "http://127.0.0.1/callback";

    explicit Tone3000Service(Deps);
    ~Tone3000Service(); // cancels downloads, waits for the pool

    std::function<void(const juce::String& name, const juce::var& data)> emit; // bridge events
    // Opens the sign-in window; done(query) with the redirect's query, done("") when the user closed it. done must be
    // called exactly once (later calls are ignored). Null or throwing = "not available".
    std::function<void(const juce::String& url, const juce::String& redirectPrefix,
                       std::function<void(const juce::String& query)> done)>
        openAuthWindow;
    std::function<void()> clearCookies;

    // All of these are message-thread calls. Every `reply` is called exactly once, possibly before returning.

    Tone3000Status status() const;
    // Opens the sign-in window and replies once it is done: {connected: true, user} or {canceled: true}, or a failure
    // ("busy", "not available", the sign-in error).
    void connect(Reply reply);
    // Opens TONE3000's picker (signing in first, in a window of its own, when not connected) and replies
    // {toneId, nodeId?} or {canceled: true}, or a failure as connect.
    void browse(const BrowseRequest&, Reply reply);
    // {tones: [ToneSummary], page, totalPages}; kind is one of the client's five lists.
    // "not connected" when signed out.
    void list(const juce::String& kind, int page, Reply reply);
    // Tone (with its A2 then A1 models). "not connected" when signed out. Nothing is cached.
    void tone(const juce::String& id, Reply reply);
    // Cancels downloads, abandons a sign-in or picker window still open (its late callback replies {canceled: true}),
    // removes the tokens, forgets the user, clears the window's cookies; emits tone3000Status.
    void disconnect();

    const ProviderIndex& index() const { return providerIndex; }

private:
    using Alive = std::shared_ptr<std::atomic<bool>>;

    // One sign-in / picker command while its window(s) are open. Abandoned by disconnect(): it then only replies
    // {canceled: true} and no longer owns `busy`.
    struct Flow
    {
        Reply reply;
        bool abandoned = false, replied = false;
    };
    using FlowPtr = std::shared_ptr<Flow>;

    // Runs work() on the pool and then done(result) on the message thread, unless the service is gone by then.
    template <typename Work, typename Done>
    void inBackground(Work work, Done done);

    FlowPtr beginFlow(Reply); // null (after replying) when busy or without a window hook
    void finish(const FlowPtr&, CommandResult);
    void openWindow(const FlowPtr&, const juce::String& url, std::function<void(const juce::String&)> onQuery);
    // Exchanges a sign-in callback; signedIn() runs (message thread) once the tokens are stored.
    void signIn(const FlowPtr&, const juce::String& query, std::function<void()> signedIn);
    void openPicker(const FlowPtr&, const BrowseRequest&);
    void pickerDone(const FlowPtr&, const BrowseRequest&, const juce::String& query);
    void stateMismatch(const FlowPtr&);
    void signOutQuietly(); // removes the tokens without the onSignedOut status event (the caller emits)
    void loadUser();
    void emitStatus();
    template <typename T>
    void replyWith(Result<T>, Reply, std::function<juce::var(const T&)> shape);

    const Deps deps;
    const Alive alive = std::make_shared<std::atomic<bool>>(true);
    FallbackSecureStore store;
    Tone3000Auth auth;
    Tone3000Client client;
    ProviderIndex providerIndex;
    DownloadManager downloads;

    bool busy = false;
    bool connected = false; // cached: the store is read at construction only
    bool clearing = false;  // signOutQuietly() is running auth.clear()
    FlowPtr flow;           // the open window flow, if any
    std::optional<UserRef> user;

    juce::ThreadPool pool { 2 }; // last member: destroyed (joined) first
};
} // namespace amp::online
