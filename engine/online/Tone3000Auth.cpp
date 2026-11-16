#include "Tone3000Auth.h"

#include "Pkce.h"

namespace amp::online
{
namespace
{
const juce::String tokensKey = "tokens";
constexpr juce::int64 refreshMarginMs = 60000;
constexpr juce::int64 defaultLifetimeSec = 3600;

juce::String formBody(const juce::StringPairArray& params) { return pkce::encodeQuery(params); }

HttpRequest formPost(const juce::String& url, const juce::StringPairArray& params)
{
    HttpRequest req;
    req.method = "POST";
    req.url = url;
    req.body = formBody(params);
    req.headers.set("Content-Type", "application/x-www-form-urlencoded");
    req.headers.set("Accept", "application/json");
    return req;
}

juce::String errorOf(const juce::String& body)
{
    auto parsed = juce::JSON::parse(body);
    if (auto* obj = parsed.getDynamicObject())
        return obj->getProperty("error").toString();
    return {};
}
} // namespace

Tone3000Auth::Tone3000Auth(AuthConfig cfg, HttpTransport& transport, SecureStore& secureStore,
                           std::function<juce::int64()> nowMs)
    : config(std::move(cfg)), http(transport), store(secureStore), clock(std::move(nowMs))
{
}

juce::int64 Tone3000Auth::now() const { return clock ? clock() : juce::Time::currentTimeMillis(); }

juce::String Tone3000Auth::authUrl(const juce::String& path) const
{
    return config.baseUrl.trimCharactersAtEnd("/") + "/api/v1/oauth/" + path;
}

std::optional<Tokens> Tone3000Auth::loadTokens() const
{
    auto raw = store.get(tokensKey);
    if (! raw)
        return std::nullopt;
    auto parsed = juce::JSON::parse(*raw);
    auto* obj = parsed.getDynamicObject();
    if (obj == nullptr)
        return std::nullopt;
    Tokens t;
    t.access = obj->getProperty("access").toString();
    t.refresh = obj->getProperty("refresh").toString();
    t.expiresAtMs = static_cast<juce::int64>(obj->getProperty("expiresAtMs"));
    if (t.access.isEmpty())
        return std::nullopt;
    return t;
}

bool Tone3000Auth::saveTokens(const Tokens& t)
{
    auto obj = std::make_unique<juce::DynamicObject>();
    obj->setProperty("access", t.access);
    obj->setProperty("refresh", t.refresh);
    obj->setProperty("expiresAtMs", t.expiresAtMs);
    return store.set(tokensKey, juce::JSON::toString(juce::var(obj.release()), true));
}

bool Tone3000Auth::parseTokens(const juce::String& body, const juce::String& keepRefresh, Tokens& out) const
{
    auto parsed = juce::JSON::parse(body);
    auto* obj = parsed.getDynamicObject();
    if (obj == nullptr)
        return false;
    out.access = obj->getProperty("access_token").toString();
    if (out.access.isEmpty())
        return false;
    out.refresh = obj->getProperty("refresh_token").toString();
    if (out.refresh.isEmpty())
        out.refresh = keepRefresh;
    auto lifetime = static_cast<juce::int64>(obj->getProperty("expires_in"));
    if (lifetime <= 0)
        lifetime = defaultLifetimeSec;
    out.expiresAtMs = now() + lifetime * 1000;
    return true;
}

AuthStart Tone3000Auth::begin(const juce::StringPairArray& extraParams)
{
    const auto verifier = pkce::randomUrlSafe(32);
    const auto state = pkce::randomUrlSafe(16);

    juce::StringPairArray q;
    q.set("response_type", "code");
    q.set("client_id", config.clientId);
    q.set("redirect_uri", config.redirectUri);
    q.set("scope", "read");
    q.set("code_challenge", pkce::challengeFor(verifier));
    q.set("code_challenge_method", "S256");
    q.set("state", state);
    for (int i = 0; i < extraParams.size(); ++i)
        q.set(extraParams.getAllKeys()[i], extraParams.getAllValues()[i]);

    {
        const juce::ScopedLock lock(pendingLock);
        pendingState = state;
        pendingVerifier = verifier;
    }
    return { authUrl("authorize") + "?" + pkce::encodeQuery(q), state };
}

CallbackResult Tone3000Auth::complete(const juce::String& query)
{
    CallbackResult result;
    const auto params = pkce::parseQuery(query);
    result.toneId = params["tone_id"];
    result.canceled = params["canceled"].equalsIgnoreCase("true") || params["canceled"] == "1";
    result.error = params["error"];
    const auto code = params["code"];
    result.hadCode = code.isNotEmpty();

    juce::String verifier;
    {
        const juce::ScopedLock lock(pendingLock);
        if (pendingState.isEmpty() || params["state"] != pendingState)
        {
            result.stateOk = false;
            return result;
        }
        verifier = pendingVerifier; // a state-matching callback consumes the flow
        pendingState = {};
        pendingVerifier = {};
    }

    if (result.error.isNotEmpty() || ! result.hadCode)
        return result;

    juce::StringPairArray form;
    form.set("grant_type", "authorization_code");
    form.set("code", code);
    form.set("client_id", config.clientId);
    form.set("code_verifier", verifier);
    form.set("redirect_uri", config.redirectUri);
    const auto response = http.request(formPost(authUrl("token"), form));

    Tokens tokens;
    if (! response.error.isEmpty())
        result.error = response.error;
    else if (! response.ok())
    {
        const auto serverError = errorOf(response.body);
        result.error = serverError.isNotEmpty() ? serverError : "token exchange failed (HTTP " + juce::String(response.status) + ")";
    }
    else if (! parseTokens(response.body, {}, tokens))
        result.error = "token response not understood";
    else if (! saveTokens(tokens))
        result.error = "could not store tokens";
    else
        result.signedIn = true;
    return result;
}

bool Tone3000Auth::isConnected() const { return loadTokens().has_value(); }

juce::String Tone3000Auth::accessToken(juce::String& error)
{
    error = {};
    auto tokens = loadTokens();
    if (! tokens)
    {
        error = "not signed in";
        return {};
    }
    if (tokens->expiresAtMs - now() >= refreshMarginMs)
        return tokens->access;

    bool signOut = false;
    juce::String result;
    {
        const juce::ScopedLock refresh(refreshLock); // later callers wait here, then find fresh tokens
        tokens = loadTokens();
        if (! tokens)
        {
            error = "signed out";
            return {};
        }
        if (tokens->expiresAtMs - now() >= refreshMarginMs)
            return tokens->access;

        if (tokens->refresh.isEmpty())
            signOut = true;
        else
        {
            juce::StringPairArray form;
            form.set("grant_type", "refresh_token");
            form.set("refresh_token", tokens->refresh);
            form.set("client_id", config.clientId);
            const auto response = http.request(formPost(authUrl("token"), form));

            if (! response.error.isEmpty())
                error = response.error;
            else if (response.status == 400 || response.status == 401 || errorOf(response.body) == "invalid_grant")
                signOut = true;
            else if (! response.ok())
                error = "token refresh failed (HTTP " + juce::String(response.status) + ")";
            else
            {
                Tokens fresh;
                if (! parseTokens(response.body, tokens->refresh, fresh))
                    error = "token response not understood";
                else if (! store.get(tokensKey)) // signed out while the request was in flight
                    error = "signed out";
                else if (! saveTokens(fresh))
                    error = "could not store tokens";
                else
                    result = fresh.access;
            }
        }
    }
    if (signOut)
    {
        clear(); // outside the refresh lock so onSignedOut never runs under one
        error = "signed out";
        return {};
    }
    return result;
}

void Tone3000Auth::clear()
{
    const bool was = store.get(tokensKey).has_value();
    store.remove(tokensKey);
    if (was && onSignedOut)
        onSignedOut();
}
} // namespace amp::online
