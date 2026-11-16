#pragma once

// Blocking HTTP transport seam for the online module: the real one wraps juce::URL, tests script a fake.

#include <functional>

#include <juce_core/juce_core.h>

namespace amp::online
{
struct HttpRequest
{
    juce::String method { "GET" }, url, body; // body is sent as application/x-www-form-urlencoded unless a
                                              // Content-Type header is given
    juce::StringPairArray headers;
    juce::File toFile; // non-empty: the body is streamed to this file, response.body stays empty
    int timeoutMs = 30000;
    std::function<void(juce::int64 received, juce::int64 total)> onProgress; // total = -1 when unknown
    std::function<bool()> shouldCancel;                                      // polled between chunks
};

struct HttpResponse
{
    int status = 0;             // 0 = no HTTP response (see error)
    juce::StringPairArray headers; // keys as sent; lookups are case-insensitive via header()
    juce::String body, error;   // error: transport failure or "canceled"

    bool ok() const { return error.isEmpty() && status >= 200 && status < 300; }

    juce::String header(const juce::String& name) const
    {
        const auto& keys = headers.getAllKeys();
        for (int i = 0; i < keys.size(); ++i)
            if (keys[i].equalsIgnoreCase(name))
                return headers.getAllValues()[i];
        return {};
    }
};

class HttpTransport
{
public:
    virtual ~HttpTransport() = default;
    virtual HttpResponse request(const HttpRequest&) = 0; // blocking, callable from any thread
};
} // namespace amp::online
