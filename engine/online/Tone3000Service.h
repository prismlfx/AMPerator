#pragma once

// The TONE3000 facade the Engine calls (tone-library spec section 5.1): connection status, connect and disconnect.
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

    static constexpr const char* redirectPrefix = "http://127.0.0.1/callback";

    explicit Tone3000Service(Deps);
    ~Tone3000Service(); // cancels downloads, waits for the pool

    std::function<void(const juce::String& name, const juce::var& data)> emit; // bridge events
    // Opens the sign-in window; done(query) with the redirect's query, done("") when the user closed it.
    // Null = "not available".
    std::function<void(const juce::String& url, const juce::String& redirectPrefix,
                       std::function<void(const juce::String& query)> done)>
        openAuthWindow;
    std::function<void()> clearCookies;

    Tone3000Status status() const;
    // Opens the sign-in window and replies once it is done: {connected: true, user} or {canceled: true}, or a failure
    // ("busy", "not available", the sign-in error). May reply before returning.
    void connect(std::function<void(CommandResult)> reply);
    // Cancels downloads, removes the tokens, forgets the user, clears the window's cookies; emits tone3000Status.
    void disconnect();

    const ProviderIndex& index() const { return providerIndex; }

private:
    using Alive = std::shared_ptr<std::atomic<bool>>;

    // Runs work() on the pool and then done(result) on the message thread, unless the service is gone by then.
    template <typename Work, typename Done>
    void inBackground(Work work, Done done);

    void finishConnect(const juce::String& query, std::function<void(CommandResult)> reply);
    void signedIn(std::function<void(CommandResult)> reply);
    void loadUser();
    void emitStatus();

    const Deps deps;
    const Alive alive = std::make_shared<std::atomic<bool>>(true);
    FallbackSecureStore store;
    Tone3000Auth auth;
    Tone3000Client client;
    ProviderIndex providerIndex;
    DownloadManager downloads;

    bool busy = false;
    bool clearing = false; // disconnect() is running auth.clear() (its own status event covers the sign-out)
    std::optional<UserRef> user;

    juce::ThreadPool pool { 2 }; // last member: destroyed (joined) first
};
} // namespace amp::online
