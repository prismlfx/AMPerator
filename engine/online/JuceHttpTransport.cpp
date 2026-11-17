#include "JuceHttpTransport.h"

#include <memory>

namespace amp::online
{
namespace http
{
namespace
{
struct Parts
{
    juce::String scheme, authority, path, query; // path starts with '/' or is empty; query without '?'
    bool valid = false;
};

bool validScheme(const juce::String& s)
{
    return s.isNotEmpty() && juce::CharacterFunctions::isLetter(s[0])
           && s.containsOnly("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789+-.");
}

// Redirect targets must not smuggle a different host through userinfo, backslashes or control characters.
bool authorityIsPlain(const juce::String& authority)
{
    for (auto c : authority)
        if (c == '\\' || c == '@' || c < 0x21 || c == 0x7f)
            return false;
    return true;
}

Parts split(const juce::String& url)
{
    Parts p;
    auto rest = url.trim();
    const auto frag = rest.indexOfChar('#');
    if (frag >= 0)
        rest = rest.substring(0, frag);
    const auto schemeEnd = rest.indexOf("://");
    if (schemeEnd <= 0)
        return p;
    if (! validScheme(rest.substring(0, schemeEnd)))
        return p;
    p.scheme = rest.substring(0, schemeEnd).toLowerCase();
    rest = rest.substring(schemeEnd + 3);
    const auto authEnd = rest.indexOfAnyOf("/?");
    p.authority = authEnd < 0 ? rest : rest.substring(0, authEnd);
    rest = authEnd < 0 ? juce::String() : rest.substring(authEnd);
    const auto q = rest.indexOfChar('?');
    p.path = q < 0 ? rest : rest.substring(0, q);
    p.query = q < 0 ? juce::String() : rest.substring(q + 1);
    p.valid = p.authority.isNotEmpty();
    return p;
}

juce::String join(const Parts& p)
{
    return p.scheme + "://" + p.authority + (p.path.isEmpty() ? "/" : p.path) + (p.query.isEmpty() ? "" : "?" + p.query);
}

// Removes "." and ".." segments from an absolute path.
juce::String normalisePath(const juce::String& path)
{
    juce::StringArray out;
    const auto segments = juce::StringArray::fromTokens(path, "/", "");
    for (int i = 0; i < segments.size(); ++i)
    {
        const auto& s = segments[i];
        const bool last = i == segments.size() - 1;
        if (s == ".")
        {
            if (last)
                out.add({});
        }
        else if (s == "..")
        {
            if (! out.isEmpty())
                out.remove(out.size() - 1);
            if (last)
                out.add({});
        }
        else
            out.add(s);
    }
    auto joined = out.joinIntoString("/");
    return joined.startsWithChar('/') ? joined : "/" + joined;
}
} // namespace

Origin originOf(const juce::String& url)
{
    Origin o;
    const auto p = split(url);
    if (! p.valid || (p.scheme != "https" && p.scheme != "http"))
        return o;
    auto auth = p.authority;
    const auto at = auth.lastIndexOfChar('@');
    if (at >= 0)
        auth = auth.substring(at + 1);
    juce::String host = auth;
    int port = p.scheme == "https" ? 443 : 80;
    const auto colon = auth.lastIndexOfChar(':');
    if (colon >= 0 && auth.indexOfChar(']') < colon) // a port, not part of an IPv6 literal
    {
        host = auth.substring(0, colon);
        const auto portText = auth.substring(colon + 1);
        if (portText.isNotEmpty())
        {
            if (! portText.containsOnly("0123456789") || portText.getIntValue() <= 0 || portText.getIntValue() > 65535)
                return o;
            port = portText.getIntValue();
        }
    }
    if (host.isEmpty())
        return o;
    o.scheme = p.scheme;
    o.host = host.toLowerCase();
    o.port = port;
    return o;
}

juce::String resolveRedirect(const juce::String& currentUrl, const juce::String& locationIn)
{
    const auto location = locationIn.trim();
    const auto base = split(currentUrl);
    if (location.isEmpty() || ! base.valid)
        return {};

    if (location.containsChar(' ') || location.containsAnyOf("\r\n"))
        return {};

    if (location.startsWith("//"))
    {
        auto p = split(base.scheme + ":" + location);
        return p.valid && authorityIsPlain(p.authority) ? join(p) : juce::String();
    }
    const auto schemeEnd = location.indexOf("://");
    if (schemeEnd > 0 && validScheme(location.substring(0, schemeEnd)))
    {
        auto p = split(location);
        return p.valid && authorityIsPlain(p.authority) ? join(p) : juce::String();
    }

    auto withoutFragment = location;
    const auto frag = withoutFragment.indexOfChar('#');
    if (frag >= 0)
        withoutFragment = withoutFragment.substring(0, frag);

    Parts r = base;
    if (withoutFragment.startsWithChar('?'))
    {
        r.query = withoutFragment.substring(1);
        return join(r);
    }
    const auto q = withoutFragment.indexOfChar('?');
    auto path = q < 0 ? withoutFragment : withoutFragment.substring(0, q);
    r.query = q < 0 ? juce::String() : withoutFragment.substring(q + 1);
    if (path.isEmpty())
        return {};
    if (! path.startsWithChar('/'))
    {
        const auto dir = base.path.substring(0, base.path.lastIndexOfChar('/') + 1);
        path = (dir.isEmpty() ? "/" : dir) + path;
    }
    r.path = normalisePath(path);
    return join(r);
}

RedirectDecision decideRedirect(const juce::String& originalUrl, const juce::String& currentUrl,
                                const juce::String& nextUrl, bool authorizationStillSent, bool bodyOnNextHop)
{
    RedirectDecision d;
    d.sendAuthorization = authorizationStillSent;
    const auto original = originOf(originalUrl), current = originOf(currentUrl), next = originOf(nextUrl);
    const auto nextParts = split(nextUrl);
    if (! next.valid() || ! authorityIsPlain(nextParts.authority))
    {
        d.action = RedirectDecision::Action::refuse;
        d.sendAuthorization = false;
        d.error = "unsupported URL";
        return d;
    }
    if (current.scheme == "https" && next.scheme == "http")
    {
        d.action = RedirectDecision::Action::refuse;
        d.sendAuthorization = false;
        d.error = "refused insecure redirect";
        return d;
    }
    if (! original.valid() || next != original)
    {
        d.sendAuthorization = false;
        if (bodyOnNextHop) // 307/308: the body (an OAuth code, a refresh token) must not leave the origin either
        {
            d.action = RedirectDecision::Action::refuse;
            d.error = "refused cross-origin redirect";
        }
    }
    return d;
}

bool isFollowedRedirectStatus(int status)
{
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

bool redirectBecomesGet(int status, const juce::String& method)
{
    return status == 303 || ((status == 301 || status == 302) && method.equalsIgnoreCase("POST"));
}
} // namespace http

namespace
{
HttpResponse failure(HttpResponse r, const juce::String& error)
{
    r.status = 0;
    r.body = {};
    r.error = error;
    return r;
}

bool hasLineBreak(const juce::String& s) { return s.containsAnyOf("\r\n"); }

juce::String headerBlock(const HttpRequest& req, bool sendAuthorization)
{
    juce::String block;
    for (int i = 0; i < req.headers.size(); ++i)
    {
        const auto name = req.headers.getAllKeys()[i].trim();
        const auto value = req.headers.getAllValues()[i];
        if (name.isEmpty() || hasLineBreak(name) || hasLineBreak(value) || name.containsChar(':'))
            continue;
        if (! sendAuthorization && name.equalsIgnoreCase("Authorization"))
            continue;
        block << name << ": " << value << "\r\n";
    }
    return block;
}

juce::String headerValue(const juce::StringPairArray& headers, const juce::String& name)
{
    for (int i = 0; i < headers.size(); ++i)
        if (headers.getAllKeys()[i].equalsIgnoreCase(name))
            return headers.getAllValues()[i];
    return {};
}

// Reads up to limit bytes; true when the stream ended within it, false when it holds more or was canceled.
// canceled is set when shouldCancel fired.
bool readCapped(juce::InputStream& in, juce::int64 limit, const HttpRequest& req, juce::MemoryBlock& out,
                bool& canceled)
{
    char buffer[JuceHttpTransport::chunkBytes];
    for (;;)
    {
        if (req.shouldCancel && req.shouldCancel())
        {
            canceled = true;
            return false;
        }
        const auto n = in.read(buffer, (int) sizeof(buffer));
        if (n <= 0)
            return true;
        if ((juce::int64) out.getSize() + n > limit)
        {
            out.append(buffer, (size_t) juce::jmax(0, (int) (limit - (juce::int64) out.getSize())));
            return false;
        }
        out.append(buffer, (size_t) n);
    }
}
} // namespace

HttpResponse JuceHttpTransport::request(const HttpRequest& req)
{
    HttpResponse res;
    if (http::originOf(req.url).valid() == false)
        return failure(res, "unsupported URL");

    auto method = req.method.isEmpty() ? juce::String("GET") : req.method.toUpperCase();
    auto body = req.body;
    auto currentUrl = req.url;
    bool sendAuthorization = true;

    for (int hop = 0;; ++hop)
    {
        if (req.shouldCancel && req.shouldCancel())
            return failure(res, "canceled");

        const bool hasBody = body.isNotEmpty() && method != "GET" && method != "HEAD";
        if (hasBody && currentUrl.containsChar('?')) // juce::URL would move the query into the POST body
            return failure(res, "unsupported URL");

        juce::URL url(currentUrl);
        if (hasBody)
            url = url.withPOSTData(body);

        juce::StringPairArray responseHeaders;
        int status = 0;
        const auto options = juce::URL::InputStreamOptions(hasBody ? juce::URL::ParameterHandling::inPostData
                                                                   : juce::URL::ParameterHandling::inAddress)
                                 .withExtraHeaders(headerBlock(req, sendAuthorization))
                                 .withConnectionTimeoutMs(req.timeoutMs)
                                 .withResponseHeaders(&responseHeaders)
                                 .withStatusCode(&status)
                                 .withHttpRequestCmd(method)
                                 .withNumRedirectsToFollow(0); // followed below, so credentials stay on their origin

        std::unique_ptr<juce::InputStream> stream = url.createInputStream(options);
        if (stream == nullptr)
            return failure(res, "connection failed");

        res.status = status;
        res.headers = responseHeaders;

        if (http::isFollowedRedirectStatus(status))
        {
            const auto location = headerValue(responseHeaders, "Location");
            if (location.isEmpty())
                return failure(res, "redirect without a location");
            if (hop >= maxRedirects)
                return failure(res, "too many redirects");

            const auto next = http::resolveRedirect(currentUrl, location);
            const bool becomesGet = http::redirectBecomesGet(status, method);
            const auto decision = http::decideRedirect(req.url, currentUrl, next, sendAuthorization,
                                                       hasBody && ! becomesGet);
            if (decision.action == http::RedirectDecision::Action::refuse)
                return failure(res, decision.error);

            sendAuthorization = decision.sendAuthorization;
            if (becomesGet)
            {
                method = "GET";
                body = {};
            }
            currentUrl = next;
            res.headers = {};
            continue;
        }

        const bool success = status >= 200 && status < 300;
        if (req.toFile != juce::File() && success)
        {
            const auto file = req.toFile;
            std::unique_ptr<juce::FileOutputStream> out;
            auto fail = [&](const juce::String& error)
            {
                out.reset(); // close before deleting
                file.deleteFile();
                return failure(res, error);
            };

            if (! file.getParentDirectory().createDirectory())
                return failure(res, "cannot create the download folder");
            file.deleteFile(); // FileOutputStream appends
            out = std::make_unique<juce::FileOutputStream>(file);
            if (out->failedToOpen())
                return failure(res, "cannot write the download file");

            auto total = headerValue(responseHeaders, "Content-Length").getLargeIntValue();
            if (total <= 0)
                total = stream->getTotalLength();
            if (total <= 0)
                total = -1;

            juce::HeapBlock<char> buffer(chunkBytes);
            juce::int64 received = 0;
            for (;;)
            {
                if (req.shouldCancel && req.shouldCancel())
                    return fail("canceled");
                const auto n = stream->read(buffer, chunkBytes);
                if (n <= 0)
                    break;
                if (! out->write(buffer, (size_t) n))
                    return fail("cannot write the download file");
                received += n;
                if (req.onProgress)
                    req.onProgress(received, total);
            }
            out->flush();
            if (out->getStatus().failed())
                return fail("cannot write the download file");
            if (total >= 0 && received < total)
                return fail("download interrupted");
            return res;
        }

        // Everything else is read into the body: the whole answer (capped), or a bounded error body for a toFile
        // request that did not succeed.
        const bool bounded = req.toFile != juce::File();
        const auto limit = bounded ? maxErrorBodyBytes : maxBodyBytes;
        juce::MemoryBlock data;
        bool canceled = false;
        const bool complete = readCapped(*stream, limit, req, data, canceled);
        if (canceled)
            return failure(res, "canceled");
        res.body = data.toString();
        if (! complete && ! bounded)
        {
            res.body = {};
            res.error = "response too large";
        }
        return res;
    }
}
} // namespace amp::online
