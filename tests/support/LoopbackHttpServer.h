#pragma once

// A tiny HTTP/1.1 server on 127.0.0.1 (ephemeral port) for transport tests: scripted replies per path, every request
// recorded. Local only, no real network. One thread accepts; each connection is served on its own thread; all are
// joined in the destructor. Replies always close the connection.

#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include <juce_core/juce_core.h>

namespace amp_test
{
class LoopbackHttpServer final : private juce::Thread
{
public:
    struct Recorded
    {
        juce::String method, path;
        juce::StringPairArray headers; // names as sent
        juce::String body;

        bool hasHeader(const juce::String& name) const
        {
            for (auto& k : headers.getAllKeys())
                if (k.equalsIgnoreCase(name))
                    return true;
            return false;
        }
    };

    struct Reply
    {
        int status = 200;
        juce::StringPairArray headers;
        std::string body;
        juce::int64 generatedBytes = -1; // >= 0: send this many 'a' bytes instead of body (cheap large bodies)
        bool declareLength = true;       // send Content-Length
        int chunkDelayMs = 0;            // sleep between 64 KiB chunks (slow body)
        juce::int64 declaredLength = -1; // override the Content-Length value
    };

    using Handler = std::function<Reply(const Recorded&)>;

    LoopbackHttpServer();
    ~LoopbackHttpServer() override;

    int port() const { return listener.getBoundPort(); }
    juce::String url(const juce::String& path = "/") const { return "http://127.0.0.1:" + juce::String(port()) + path; }

    void on(const juce::String& path, Handler h);
    void on(const juce::String& path, Reply r);

    std::vector<Recorded> requests() const;
    std::vector<Recorded> requestsFor(const juce::String& path) const;

private:
    void run() override;
    void serve(std::unique_ptr<juce::StreamingSocket> socket);

    juce::StreamingSocket listener;
    mutable std::mutex lock;
    std::map<juce::String, Handler> handlers;
    std::vector<Recorded> seen;
    std::vector<std::thread> workers;
    std::atomic<bool> stopping { false };
};
} // namespace amp_test
