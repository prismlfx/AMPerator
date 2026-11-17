#pragma once

// The real HttpTransport over juce::URL (NSURLSession on macOS). Stateless and thread-safe. Redirects are followed
// here, not by JUCE, so an Authorization header can never reach another origin. Every request carries
// "Cache-Control: no-store" unless it sets its own.

#include "HttpTransport.h"

namespace amp::online
{
// Limit: JUCE reports a connection dropped mid-body as a clean end of stream. With a Content-Length the transport
// detects the shortfall ("download interrupted"); without one a truncated download looks like a success, so callers
// must validate downloaded content (DownloadManager does via validateAssetFile).
class JuceHttpTransport final : public HttpTransport
{
public:
    static constexpr int maxRedirects = 5;
    static constexpr juce::int64 maxBodyBytes = 8 * 1024 * 1024; // bodies not streamed to a file
    static constexpr juce::int64 maxErrorBodyBytes = 64 * 1024;  // body kept for a non-2xx answer to a toFile request
    static constexpr int chunkBytes = 64 * 1024;

    HttpResponse request(const HttpRequest&) override;
};

namespace http
{
// scheme://host:port of an http(s) URL, lower-cased, default ports filled in. valid() is false for any other URL.
struct Origin
{
    juce::String scheme, host;
    int port = 0;

    bool valid() const { return scheme.isNotEmpty() && host.isNotEmpty(); }
    bool operator==(const Origin& o) const { return scheme == o.scheme && host == o.host && port == o.port; }
    bool operator!=(const Origin& o) const { return ! (*this == o); }
};

Origin originOf(const juce::String& url);

// Whether the URL parses and its authority has no userinfo, backslash or control characters (the same rule as for
// redirect targets). A URL failing it is refused before any connection, and never gets a token.
bool hasPlainAuthority(const juce::String& url);

// Resolves a Location header against the URL that answered. Absolute, protocol-relative ("//host/x"), absolute-path,
// relative and query-only references are handled; the fragment is dropped. Empty when it cannot be resolved.
juce::String resolveRedirect(const juce::String& currentUrl, const juce::String& location);

struct RedirectDecision
{
    enum class Action { follow, refuse };
    Action action = Action::follow;
    bool sendAuthorization = true; // for the next hop and, once false, every later hop
    juce::String error;            // set when action == refuse
};

// Decides what to do with a hop to nextUrl. originalUrl is the URL the caller asked for; authorizationStillSent is
// whether the hop that was just answered still carried the Authorization header. A hop to another scheme/host/port
// than the original drops the header for good; an https -> http hop is refused; a non-http(s) target (or one whose
// authority has userinfo, a backslash or control characters) is refused as "unsupported URL". bodyOnNextHop: the hop
// would still send the request body (307/308 on a POST); to another origin that is refused ("refused cross-origin
// redirect") because the body can carry an authorization code or refresh token.
RedirectDecision decideRedirect(const juce::String& originalUrl, const juce::String& currentUrl,
                                const juce::String& nextUrl, bool authorizationStillSent, bool bodyOnNextHop);

// 301, 302, 303, 307, 308.
bool isFollowedRedirectStatus(int status);

// Whether the follow-up request is a GET without a body: 303 always, 301/302 when the method was POST.
bool redirectBecomesGet(int status, const juce::String& method);
} // namespace http
} // namespace amp::online
