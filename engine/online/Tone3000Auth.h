#pragma once

// TONE3000 OAuth 2.0 + PKCE: authorize URL, callback handling, token exchange, refresh and persistence.
// begin / isConnected / clear are message-thread calls; complete / accessToken block and run on workers.

#include <functional>

#include <juce_core/juce_core.h>

#include "HttpTransport.h"
#include "SecureStore.h"

namespace amp::online
{
struct AuthConfig
{
    juce::String clientId, baseUrl { "https://www.tone3000.com" }, redirectUri { "http://127.0.0.1/callback" };
};

// Stored under the key "tokens" as JSON {access, refresh, expiresAtMs}.
struct Tokens
{
    juce::String access, refresh;
    juce::int64 expiresAtMs = 0;
};

struct AuthStart
{
    juce::String url, state;
};

struct CallbackResult
{
    bool stateOk = true, canceled = false, hadCode = false, signedIn = false;
    juce::String toneId, error;
};

class Tone3000Auth
{
public:
    Tone3000Auth(AuthConfig, HttpTransport&, SecureStore&, std::function<juce::int64()> nowMs = {});

    // Builds the authorize URL; remembers state + verifier (one pending flow, a new begin() replaces it).
    AuthStart begin(const juce::StringPairArray& extraParams = {});
    // Validates state, reads tone_id / canceled / error, exchanges `code` and stores the tokens. Blocking.
    CallbackResult complete(const juce::String& query);
    bool isConnected() const;
    // Valid access token, refreshing when under 60 s remain (single-flight). "" + error on failure. Blocking.
    juce::String accessToken(juce::String& error);
    // Removes the stored tokens; fires onSignedOut if they were present.
    void clear();

    std::function<void()> onSignedOut; // any thread, never invoked under a lock

private:
    juce::int64 now() const;
    std::optional<Tokens> loadTokens() const;
    bool saveTokens(const Tokens&);
    // Parses a token endpoint body; keepRefresh is used when the response carries no refresh_token.
    bool parseTokens(const juce::String& body, const juce::String& keepRefresh, Tokens& out) const;
    juce::String authUrl(const juce::String& path) const;

    AuthConfig config;
    HttpTransport& http;
    SecureStore& store;
    std::function<juce::int64()> clock;

    juce::CriticalSection pendingLock; // guards state / verifier only; never held across network I/O
    juce::String pendingState, pendingVerifier;
    juce::CriticalSection refreshLock; // makes refresh single-flight
};
} // namespace amp::online
