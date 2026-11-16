#pragma once

// Typed, blocking REST client for the free-tier TONE3000 endpoints. Every call runs on a worker thread; it adds the
// Bearer token from Tone3000Auth, retries once on 401, and maps failures to ClientError. 429 is never retried.

#include <functional>

#include <juce_core/juce_core.h>

#include "HttpTransport.h"
#include "Tone3000Auth.h"
#include "Tone3000Types.h"

namespace amp::online
{
class Tone3000Client
{
public:
    Tone3000Client(Tone3000Auth&, HttpTransport&, juce::String baseUrl);

    Result<UserRef> user();                                // GET /api/v1/user
    Result<Page> list(const juce::String& kind, int page); // favorited | downloaded | created | trending | latest
    Result<Tone> tone(const juce::String& id);             // the tone plus its A2 then A1 models ("custom" dropped)
    Result<ModelInfo> model(const juce::String& id);
    // GET `url` streamed to `to`. The Bearer header is sent only when the URL's host is the base URL's host.
    ClientError fetch(const juce::String& url, const juce::File& to,
                      std::function<void(juce::int64, juce::int64)> progress, std::function<bool()> cancel);

    std::function<void(const juce::String&)> onDeprecation; // X-Tone3000-Deprecations value, any thread

private:
    struct Reply
    {
        HttpResponse response;
        ClientError error;
    };

    // Sends `req` with auth (when `authorize`), the 401 retry and status mapping.
    Reply send(HttpRequest req, bool authorize);
    Reply getJson(const juce::String& path, juce::var& parsed);
    juce::String apiUrl(const juce::String& path) const;

    Tone3000Auth& auth;
    HttpTransport& http;
    juce::String base;
};
} // namespace amp::online
