#include "Tone3000Client.h"

namespace amp::online
{
namespace
{
ClientError makeError(ClientError::Kind kind, const juce::String& message, int retryAfterSec = 0)
{
    ClientError e;
    e.kind = kind;
    e.message = message;
    e.retryAfterSec = retryAfterSec;
    return e;
}

// Unwraps a {data: ...} envelope.
juce::var unwrap(const juce::var& v)
{
    if (v.isObject() && v.getProperty("data", {}).isObject())
        return v.getProperty("data", {});
    return v;
}

juce::var dataArray(const juce::var& v)
{
    if (v.isArray())
        return v;
    return v.getProperty("data", {});
}

int effectivePort(const juce::URL& u)
{
    return u.getPort() > 0 ? u.getPort() : (u.getScheme().equalsIgnoreCase("https") ? 443 : 80);
}

// The token goes only to the configured origin: same scheme (never cleartext http), host and port.
bool sameOrigin(const juce::URL& a, const juce::URL& b)
{
    return a.getScheme().equalsIgnoreCase(b.getScheme()) && a.getDomain().equalsIgnoreCase(b.getDomain())
           && effectivePort(a) == effectivePort(b);
}

juce::String escaped(const juce::String& s) { return juce::URL::addEscapeChars(s, true); }
} // namespace

Tone3000Client::Tone3000Client(Tone3000Auth& a, HttpTransport& h, juce::String baseUrl)
    : auth(a), http(h), base(baseUrl.trimCharactersAtEnd("/"))
{
}

juce::String Tone3000Client::apiUrl(const juce::String& path) const { return base + "/api/v1" + path; }

Tone3000Client::Reply Tone3000Client::send(HttpRequest req, bool authorize)
{
    Reply reply;
    bool retried = false;

    for (;;)
    {
        if (authorize)
        {
            juce::String tokenError;
            const auto token = auth.accessToken(tokenError, retried); // the 401 retry forces a refresh
            if (token.isEmpty())
            {
                reply.error = tokenError.containsIgnoreCase("signed")
                                  ? makeError(ClientError::Kind::NotConnected, "not connected")
                                  : makeError(ClientError::Kind::Network, tokenError.isEmpty() ? "no access token" : tokenError);
                return reply;
            }
            req.headers.set("Authorization", "Bearer " + token);
        }

        reply.response = http.request(req);
        const auto& r = reply.response;

        if (onDeprecation)
            if (const auto deprecation = r.header("X-Tone3000-Deprecations"); deprecation.isNotEmpty())
                onDeprecation(deprecation);

        if (r.error.isNotEmpty())
            reply.error = makeError(ClientError::Kind::Network, r.error);
        else if (r.status == 401 && authorize && ! retried)
        {
            retried = true; // one forced refresh and one retry, never more
            continue;
        }
        else if (r.status == 429)
            reply.error = makeError(ClientError::Kind::RateLimited, "rate limited",
                                    juce::jmax(0, r.header("Retry-After").getIntValue()));
        else if (r.status == 404)
            reply.error = makeError(ClientError::Kind::NotFound, "not found");
        else if (r.status == 403)
            reply.error = makeError(ClientError::Kind::Forbidden, "forbidden");
        else if (r.status < 200 || r.status >= 300)
            reply.error = makeError(ClientError::Kind::Http, "HTTP " + juce::String(r.status));
        return reply;
    }
}

Tone3000Client::Reply Tone3000Client::getJson(const juce::String& path, juce::var& parsed)
{
    HttpRequest req;
    req.url = apiUrl(path);
    req.headers.set("Accept", "application/json");

    auto reply = send(std::move(req), true);
    if (reply.error)
        return reply;

    if (! juce::JSON::parse(reply.response.body, parsed) || ! (parsed.isObject() || parsed.isArray()))
        reply.error = makeError(ClientError::Kind::Parse, "unparsable response");
    return reply;
}

Result<UserRef> Tone3000Client::user()
{
    Result<UserRef> out;
    juce::var json;
    out.error = getJson("/user", json).error;
    if (! out.error)
        out.value = parseUser(unwrap(json));
    return out;
}

Result<Page> Tone3000Client::list(const juce::String& kind, int page)
{
    Result<Page> out;
    const bool paged = kind == "favorited" || kind == "downloaded" || kind == "created";
    if (! paged && kind != "trending" && kind != "latest")
    {
        out.error = makeError(ClientError::Kind::Http, "unknown list: " + kind);
        return out;
    }

    const int pageNumber = juce::jmax(1, page);
    const auto path = "/tones/" + kind + (paged ? "?page=" + juce::String(pageNumber) + "&page_size=25" : juce::String());

    juce::var json;
    out.error = getJson(path, json).error;
    if (out.error)
        return out;

    Page result;
    if (auto* arr = dataArray(json).getArray())
        for (const auto& item : *arr)
            result.tones.push_back(parseToneSummary(item));
    if (paged)
    {
        result.page = juce::jmax(1, jsonInt(json.getProperty("page", {}), pageNumber));
        result.totalPages = juce::jmax(1, jsonInt(json.getProperty("total_pages", {}), 1));
    }
    out.value = std::move(result);
    return out;
}

Result<Tone> Tone3000Client::tone(const juce::String& id)
{
    Result<Tone> out;
    juce::var json;
    out.error = getJson("/tones/" + escaped(id), json).error;
    if (out.error)
        return out;

    auto result = parseTone(unwrap(json));
    if (result.id.isEmpty())
        result.id = id;

    // Only the first page (page_size=300) is read; a tone has far fewer models than that.
    // NAM: A2 models only come back when asked for explicitly, so ask for A2 first and then A1. Other formats (IR)
    // have a null architecture_version and the filter's effect on them is undocumented: one unfiltered request.
    std::vector<juce::String> queries;
    const auto prefix = "/models?tone_id=" + escaped(id);
    if (result.format == "nam")
        queries = { prefix + "&architecture=2&page_size=300", prefix + "&architecture=1&page_size=300" };
    else
        queries = { prefix + "&page_size=300" };

    for (const auto& query : queries)
    {
        juce::var models;
        out.error = getJson(query, models).error;
        if (out.error)
            return out;

        if (auto* arr = dataArray(models).getArray())
            for (const auto& item : *arr)
            {
                auto model = parseModel(item);
                if (model.architecture == "custom" || model.id.isEmpty())
                    continue;
                const bool seen = std::any_of(result.models.begin(), result.models.end(),
                                              [&](const ModelInfo& m) { return m.id == model.id; });
                if (seen)
                    continue;
                if (model.toneId.isEmpty())
                    model.toneId = result.id;
                result.models.push_back(std::move(model));
            }
    }
    out.value = std::move(result);
    return out;
}

Result<ModelInfo> Tone3000Client::model(const juce::String& id)
{
    Result<ModelInfo> out;
    juce::var json;
    out.error = getJson("/models/" + escaped(id), json).error;
    if (! out.error)
        out.value = parseModel(unwrap(json));
    return out;
}

ClientError Tone3000Client::fetch(const juce::String& url, const juce::File& to,
                                  std::function<void(juce::int64, juce::int64)> progress, std::function<bool()> cancel)
{
    HttpRequest req;
    req.url = url;
    req.toFile = to;
    req.onProgress = std::move(progress);
    req.shouldCancel = std::move(cancel);
    req.headers.set("Accept", "*/*");
    req.timeoutMs = 120000;

    // Never leak the token to another host.
    const bool ours = sameOrigin(juce::URL(url), juce::URL(base));
    auto reply = send(std::move(req), ours);

    if (reply.error && reply.response.status != 0)
        to.deleteFile(); // an error body is not a model
    return reply.error;
}
} // namespace amp::online
