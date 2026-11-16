#pragma once

// Scripted HttpTransport for tests: register per-method URL-prefix handlers with on(); the first registered
// match answers, anything else gets status 0 and "no fake response for <method> <url>". Thread-safe: worker
// threads may call request() while the test registers handlers. Handlers run without the lock held.

#include <functional>
#include <mutex>
#include <vector>

#include "online/HttpTransport.h"

namespace amp_test
{
class FakeHttp final : public amp::online::HttpTransport
{
public:
    using Handler = std::function<amp::online::HttpResponse(const amp::online::HttpRequest&)>;

    void on(const juce::String& method, const juce::String& urlPrefix, Handler handler);
    amp::online::HttpResponse request(const amp::online::HttpRequest& req) override;

    // Every request seen, in order. Read it only when no request is in flight (or copy under your own sync).
    std::vector<amp::online::HttpRequest> requests;

    static amp::online::HttpResponse json(int status, const juce::String& body, juce::StringPairArray headers = {});
    // Writes the bytes to toFile when the request has one (the transport contract); body stays empty then.
    static amp::online::HttpResponse file(int status, const juce::MemoryBlock& bytes);

private:
    struct Route
    {
        juce::String method, prefix;
        Handler handler;
    };
    std::mutex lock;
    std::vector<Route> routes;
};
} // namespace amp_test
