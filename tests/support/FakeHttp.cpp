#include "FakeHttp.h"

#include <map>

namespace amp_test
{
using amp::online::HttpRequest;
using amp::online::HttpResponse;

// file() cannot see the request, so it parks its bytes in a process-wide registry under an id carried by an
// internal header; request() resolves the id, writes toFile (or fills body) and strips the header. Works for
// responses built ahead of time or on another thread.
namespace
{
const char* const fileIdHeader = "X-Fake-File-Id";
std::mutex fileLock;
std::map<juce::int64, juce::MemoryBlock> fileRegistry;
juce::int64 nextFileId = 1;
}

std::vector<HttpRequest> FakeHttp::snapshot() const
{
    std::lock_guard<std::mutex> guard(lock);
    return requests;
}

size_t FakeHttp::requestCount() const
{
    std::lock_guard<std::mutex> guard(lock);
    return requests.size();
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
    auto response = handler(req);
    const auto id = response.header(fileIdHeader);
    if (id.isNotEmpty())
    {
        juce::MemoryBlock bytes;
        {
            std::lock_guard<std::mutex> guard(fileLock);
            auto it = fileRegistry.find(id.getLargeIntValue());
            if (it != fileRegistry.end())
                bytes = it->second; // kept: a captured response may be served more than once (tiny test leak)
        }
        auto stripped = juce::StringPairArray();
        for (int i = 0; i < response.headers.size(); ++i)
            if (! response.headers.getAllKeys()[i].equalsIgnoreCase(fileIdHeader))
                stripped.set(response.headers.getAllKeys()[i], response.headers.getAllValues()[i]);
        response.headers = stripped;
        if (req.toFile != juce::File())
        {
            req.toFile.getParentDirectory().createDirectory();
            if (! req.toFile.replaceWithData(bytes.getData(), bytes.getSize()))
                response.error = "could not write " + req.toFile.getFullPathName();
            response.body = {};
        }
        else
            response.body = bytes.toString();
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
    std::lock_guard<std::mutex> guard(fileLock);
    const auto id = nextFileId++;
    fileRegistry[id] = bytes;
    r.headers.set(fileIdHeader, juce::String(id));
    return r;
}
} // namespace amp_test
