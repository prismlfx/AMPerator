#include "Tone3000Auth.h"

#include "Pkce.h"

namespace amp::online
{
namespace
{
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
    auto raw = store.get(config.storeKey);
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
    return store.set(config.storeKey, juce::JSON::toString(juce::var(obj.release()), true));
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
    static const juce::StringArray reserved { "response_type", "client_id", "redirect_uri", "scope", "code_challenge",
                                              "code_challenge_method", "state" };
    for (int i = 0; i < extraParams.size(); ++i)
        if (! reserved.contains(extraParams.getAllKeys()[i]))
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
    else
    {
        const juce::ScopedLock state(stateLock);
        if (! saveTokens(tokens))
            result.error = "could not store tokens";
        else
            result.signedIn = true;
    }
    return result;
}

bool Tone3000Auth::isConnected() const { return loadTokens().has_value(); }

juce::String Tone3000Auth::accessToken(juce::String& error, bool forceRefresh)
{
    error = {};
    auto tokens = loadTokens();
    if (! tokens)
    {
        error = "not signed in";
        return {};
    }
    if (! forceRefresh && tokens->expiresAtMs - now() >= refreshMarginMs)
        return tokens->access;
    const auto enteredWith = tokens->access;

    bool signedOut = false;
    juce::String result;
    {
        const juce::ScopedLock refresh(refreshLock); // later callers wait here, then find fresh tokens
        tokens = loadTokens();
        if (! tokens)
        {
            error = "signed out";
            return {};
        }
        if (forceRefresh ? tokens->access != enteredWith : tokens->expiresAtMs - now() >= refreshMarginMs)
            return tokens->access;

        if (tokens->refresh.isEmpty())
        {
            const juce::ScopedLock state(stateLock);
            signedOut = removeIfStillHolding(tokens->refresh);
            error = "signed out";
        }
        else
        {
            juce::StringPairArray form;
            form.set("grant_type", "refresh_token");
            form.set("refresh_token", tokens->refresh);
            form.set("client_id", config.clientId);
            const auto response = http.request(formPost(authUrl("token"), form));

            Tokens fresh;
            bool revoked = false, parsed = false;
            if (! response.error.isEmpty())
                error = response.error;
            else if (response.status == 400 || response.status == 401 || errorOf(response.body) == "invalid_grant")
                revoked = true;
            else if (! response.ok())
                error = "token refresh failed (HTTP " + juce::String(response.status) + ")";
            else if (! parseTokens(response.body, tokens->refresh, fresh))
                error = "token response not understood";
            else
                parsed = true;

            if (revoked || parsed)
            {
                // Only touch the store if it still holds the tokens this refresh started from: the user may have
                // signed out, or in again, while the request was in flight.
                const juce::ScopedLock state(stateLock);
                const auto stored = loadTokens();
                if (! stored)
                    error = "signed out";
                else if (stored->refresh != tokens->refresh)
                    result = stored->access; // a newer sign-in won; use it, leave it alone
                else if (revoked)
                {
                    store.remove(config.storeKey);
                    signedOut = true;
                    error = "signed out";
                }
                else if (saveTokens(fresh))
                    result = fresh.access;
                else
                    error = "could not store tokens";
            }
        }
    }
    if (signedOut && onSignedOut)
        onSignedOut(); // outside every lock
    return result;
}

// Caller holds stateLock. Removes the tokens when they still carry `refresh`; true if it removed them.
bool Tone3000Auth::removeIfStillHolding(const juce::String& refresh)
{
    const auto stored = loadTokens();
    if (! stored || stored->refresh != refresh)
        return false;
    store.remove(config.storeKey);
    return true;
}

void Tone3000Auth::clear()
{
    bool was;
    {
        const juce::ScopedLock state(stateLock);
        was = loadTokens().has_value();
        store.remove(config.storeKey);
    }
    if (was && onSignedOut)
        onSignedOut();
}
} // namespace amp::online
