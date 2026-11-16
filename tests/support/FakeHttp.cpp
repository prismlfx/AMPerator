#include "FakeHttp.h"

#include <optional>

namespace amp_test
{
using amp::online::HttpRequest;
using amp::online::HttpResponse;

// file() runs inside a handler on the requesting thread and cannot see the request, so it parks its bytes here
// and request() (same thread, right after the handler returns) writes them to the request's toFile.
namespace
{
thread_local std::optional<juce::MemoryBlock> pendingFileBytes;
}

void FakeHttp::on(const juce::String& method, const juce::String& urlPrefix, Handler handler)
{
    std::lock_guard<std::mutex> guard(lock);
    routes.push_back({ method, urlPrefix, std::move(handler) });
}

HttpResponse FakeHttp::request(const HttpRequest& req)
{
    Handler handler;
    {
        std::lock_guard<std::mutex> guard(lock);
        requests.push_back(req);
        for (const auto& r : routes)
            if (r.method.equalsIgnoreCase(req.method) && req.url.startsWith(r.prefix))
            {
                handler = r.handler;
                break;
            }
    }
    if (! handler)
    {
        HttpResponse miss;
        miss.error = "no fake response for " + req.method + " " + req.url;
        return miss;
    }
    pendingFileBytes.reset();
    auto response = handler(req);
    if (pendingFileBytes.has_value())
    {
        if (req.toFile != juce::File())
        {
            req.toFile.getParentDirectory().createDirectory();
            if (! req.toFile.replaceWithData(pendingFileBytes->getData(), pendingFileBytes->getSize()))
                response.error = "could not write " + req.toFile.getFullPathName();
            response.body = {};
        }
        pendingFileBytes.reset();
    }
    return response;
}

HttpResponse FakeHttp::json(int status, const juce::String& body, juce::StringPairArray headers)
{
    HttpResponse r;
    r.status = status;
    r.body = body;
    r.headers = std::move(headers);
    if (r.header("Content-Type").isEmpty())
        r.headers.set("Content-Type", "application/json");
    return r;
}

HttpResponse FakeHttp::file(int status, const juce::MemoryBlock& bytes)
{
    HttpResponse r;
    r.status = status;
    r.body = bytes.toString(); // what a request without toFile sees
    pendingFileBytes = bytes;
    return r;
}
} // namespace amp_test
