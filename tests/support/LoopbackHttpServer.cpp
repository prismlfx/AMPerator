#include "LoopbackHttpServer.h"

#include <cerrno>

namespace amp_test
{
namespace
{
bool readSome(juce::StreamingSocket& s, std::string& into, std::atomic<bool>& stopping)
{
    char buf[4096];
    for (int waited = 0; waited < 5000 && ! stopping; waited += 50)
    {
        if (s.waitUntilReady(true, 50) == 1)
        {
            const auto n = s.read(buf, sizeof(buf), false);
            if (n <= 0)
                return false;
            into.append(buf, (size_t) n);
            return true;
        }
    }
    return false;
}

bool writeAll(juce::StreamingSocket& s, const char* data, size_t size)
{
    size_t done = 0;
    while (done < size)
    {
        const auto n = s.write(data + done, (int) juce::jmin<size_t>(size - done, 1 << 20));
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) // accepted sockets can be non-blocking
        {
            juce::Thread::sleep(1);
            continue;
        }
        if (n <= 0)
            return false;
        done += (size_t) n;
    }
    return true;
}
} // namespace

LoopbackHttpServer::LoopbackHttpServer() : juce::Thread("loopback http")
{
    const bool ok = listener.createListener(0, "127.0.0.1");
    jassert(ok);
    juce::ignoreUnused(ok);
    startThread();
}

LoopbackHttpServer::~LoopbackHttpServer()
{
    stopping = true;
    stopThread(5000);
    listener.close();
    std::vector<std::thread> toJoin;
    {
        std::lock_guard<std::mutex> g(lock);
        toJoin.swap(workers);
    }
    for (auto& t : toJoin)
        t.join();
}

void LoopbackHttpServer::on(const juce::String& path, Handler h)
{
    std::lock_guard<std::mutex> g(lock);
    handlers[path] = std::move(h);
}

void LoopbackHttpServer::on(const juce::String& path, Reply r)
{
    on(path, [r](const Recorded&) { return r; });
}

std::vector<LoopbackHttpServer::Recorded> LoopbackHttpServer::requests() const
{
    std::lock_guard<std::mutex> g(lock);
    return seen;
}

std::vector<LoopbackHttpServer::Recorded> LoopbackHttpServer::requestsFor(const juce::String& path) const
{
    std::vector<Recorded> out;
    for (auto& r : requests())
        if (r.path == path)
            out.push_back(r);
    return out;
}

void LoopbackHttpServer::run()
{
    while (! threadShouldExit() && ! stopping)
    {
        if (listener.waitUntilReady(true, 50) != 1)
            continue;
        std::unique_ptr<juce::StreamingSocket> conn(listener.waitForNextConnection());
        if (conn == nullptr)
            continue;
        std::lock_guard<std::mutex> g(lock);
        workers.emplace_back([this, c = std::move(conn)]() mutable { serve(std::move(c)); });
    }
}

void LoopbackHttpServer::serve(std::unique_ptr<juce::StreamingSocket> socket)
{
    std::string raw;
    size_t headEnd = std::string::npos;
    while ((headEnd = raw.find("\r\n\r\n")) == std::string::npos)
        if (! readSome(*socket, raw, stopping))
            return;

    Recorded rec;
    const auto head = juce::String(raw.substr(0, headEnd));
    auto lines = juce::StringArray::fromLines(head);
    const auto start = juce::StringArray::fromTokens(lines[0], " ", "");
    rec.method = start[0];
    rec.path = start[1];
    for (int i = 1; i < lines.size(); ++i)
    {
        const auto colon = lines[i].indexOfChar(':');
        if (colon > 0)
            rec.headers.set(lines[i].substring(0, colon), lines[i].substring(colon + 1).trim());
    }
    size_t want = 0;
    for (auto& k : rec.headers.getAllKeys())
        if (k.equalsIgnoreCase("Content-Length"))
            want = (size_t) rec.headers[k].getLargeIntValue();
    std::string body = raw.substr(headEnd + 4);
    while (body.size() < want)
        if (! readSome(*socket, body, stopping))
            return;
    rec.body = juce::String(body);

    Handler handler;
    {
        std::lock_guard<std::mutex> g(lock);
        seen.push_back(rec);
        auto it = handlers.find(rec.path);
        if (it != handlers.end())
            handler = it->second;
    }
    Reply reply;
    if (handler)
        reply = handler(rec);
    else
        reply.status = 404;

    const juce::int64 length = reply.generatedBytes >= 0 ? reply.generatedBytes : (juce::int64) reply.body.size();
    juce::String out;
    out << "HTTP/1.1 " << reply.status << " X\r\nConnection: close\r\n";
    for (int i = 0; i < reply.headers.size(); ++i)
        out << reply.headers.getAllKeys()[i] << ": " << reply.headers.getAllValues()[i] << "\r\n";
    if (reply.declareLength)
        out << "Content-Length: " << (reply.declaredLength >= 0 ? reply.declaredLength : length) << "\r\n";
    out << "\r\n";
    const auto headText = out.toStdString();
    if (! writeAll(*socket, headText.data(), headText.size()))
        return;

    constexpr size_t chunk = 64 * 1024;
    const std::string filler(chunk, 'a');
    juce::int64 sent = 0;
    while (sent < length && ! stopping)
    {
        const auto n = (size_t) juce::jmin<juce::int64>((juce::int64) chunk, length - sent);
        const bool ok = reply.generatedBytes >= 0 ? writeAll(*socket, filler.data(), n)
                                                  : writeAll(*socket, reply.body.data() + sent, n);
        if (! ok)
            return;
        sent += (juce::int64) n;
        if (reply.chunkDelayMs > 0)
            juce::Thread::sleep(reply.chunkDelayMs);
    }

    // Closing right away can reset the connection and drop unread data on the client: wait for the client to hang up.
    std::string ignored;
    for (int waited = 0; waited < 2000 && ! stopping; waited += 50)
        if (socket->waitUntilReady(true, 50) == 1)
        {
            char c[256];
            if (socket->read(c, sizeof(c), false) <= 0)
                break;
        }
}
} // namespace amp_test
