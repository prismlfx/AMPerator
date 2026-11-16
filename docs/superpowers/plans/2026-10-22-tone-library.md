# Tone Library (TONE3000) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Browse TONE3000 from inside AMPerator, download a chosen NAM model or IR into the library with its attribution, play it, and restore a rig's missing TONE3000 files with one click.

**Architecture:** A new headless module `engine/online/` (PKCE auth, typed REST client over an injectable HTTP transport, provenance index, download jobs on a thread pool, one façade `Tone3000Service`) is wired into `Engine` and exposed as additive `tone3000*` bridge commands and events; long commands reply later through a new deferred-reply mechanism in `Bridge`. The app contributes one window (an in-app web view that intercepts the OAuth redirect) and an `openExternal` native function. The web UI gains a TONE3000 library tab, a tone detail modal, node menu entries, badges and a load banner.

**Tech Stack:** C++20, JUCE 8 (`juce::URL` for HTTP, `juce::ThreadPool`, `juce::WebBrowserComponent`), macOS Security.framework (Keychain), Catch2 v3, Vite + TypeScript + vitest (no UI framework). No new third-party libraries.

**Spec:** `docs/superpowers/specs/2026-10-20-tone-library-design.md`

## Global Constraints

- Namespace `amp::online` for everything under `engine/online/`; engine stays headless (no windows, no `juce_gui_*` types) and message-thread-only at its public surface; network work on worker threads, results posted with `juce::MessageManager::callAsync`.
- The audio thread is never touched by this sub-project.
- Bridge protocol version stays `1`; every new command/field is additive and documented in `docs/bridge-protocol.md`.
- TONE3000 base URL `https://www.tone3000.com`, API prefix `/api/v1`, redirect URI `http://127.0.0.1/callback`, scope `read`, PKCE method `S256`.
- Free-tier only: the client may call `/oauth/authorize`, `/oauth/token`, `/user`, `/tones/{favorited|downloaded|created|trending|latest}`, `/tones/{id}`, `/models?tone_id=`, `/models/{id}`, and `model_url`. No `/tones/search`, `/makes`, `/tags`, `/users`, no favoriting. Any other path in the client is a bug.
- Downloads happen only from `tone3000Download` / `tone3000Fetch` (one user action, or the opt-in `tone3000AutoFetch` setting when already connected); nothing is prefetched; list results are never written to disk.
- Downloaded files: `Models/TONE3000/<creator>/<tone title> — <model name>.nam`, `IRs/TONE3000/<creator>/<tone title> — <model name>.wav` (path components through `RigStore::sanitizeFileName`; a name clash with a different sha appends ` (2)`, ` (3)`, …). Partials in `<dataDir>/tone3000/.partial/<jobId>`. Provenance in `<dataDir>/tone3000/index.json`. Images in `<libraryRoot>/Art/tone3000/<toneId>.<ext>`.
- Rig file: `asset.source = {"provider": "tone3000", "id": "<model id>"}`; rig format version stays 1.
- Secure storage key: Keychain service `com.amperator.tone3000`, account = the client id. Tokens never go to `settings.json` or any file.
- Rate limits are honoured: a 429 is surfaced with its `Retry-After`, never retried automatically; refresh at most once per failing request; refresh single-flight, started when < 60 s of life remain.
- Branding copy fixed by the spec: tab name `TONE3000`; splash copy "AMPerator has partnered with TONE3000 to give you access to thousands of NAM captures and IRs."; "Powered by TONE3000" with link `https://www.tone3000.com`; buttons `Connect TONE3000`, `Browse TONE3000`, `Discover more on TONE3000`, `View on TONE3000`, node menu `Browse TONE3000…`, `Replace from TONE3000…`, banner `N models from TONE3000 are missing.` + `Download`.
- Built-in archetype art stays free of brand names; TONE3000 logos live only in `ui/src/brand/tone3000/` and are listed in `THIRD_PARTY.md` as trademarks used under TONE3000's brand guidelines.
- Client id: CMake cache variable `AMP_TONE3000_CLIENT_ID` (default empty) → compile definition of the same name on `amp_engine`; `settings.json` `tone3000ClientId` overrides at runtime; empty → every `tone3000*` command replies `ok:false "not configured"`.
- Commit after every task; TDD throughout (failing test first).

## Review Focus

1. A tone title or creator name with `/`, `:`, emoji or 200 characters: the file lands inside `Models/TONE3000/<creator>/` with a sane name, no stray folders, and the title in the UI is unchanged. → Task 8 test `placement sanitizes creator and title`.
2. The OAuth callback query arrives URL-encoded, with parameters in any order, possibly `canceled=true` *and* a `code`: state is checked after decoding, the code is still exchanged, `canceled` is reported. → Task 5 tests `callback parameters are decoded` and `canceled callback with a code still signs in`.
3. `model_url` answers 200 with an HTML sign-in page (expired session mid-flight): the file is rejected as "not a valid nam file", nothing enters the library, the job fails cleanly. → Task 9 test `html body is not accepted as a model`.
4. The same model is requested twice before the first finishes (double click, or a rig with the same model on two nodes): one file on disk, both jobs reach `done`, both nodes assigned. → Task 13 test `duplicate downloads share one file`.
5. Offline (transport error) while clicking Download in the banner: the job fails with the transport message, `busy` never sticks, the banner stays so the user can retry. → Task 13 test `fetch while offline fails each job and keeps fetchable`.

---

## File Structure

```
engine/online/
  HttpTransport.h             HttpRequest / HttpResponse / HttpTransport interface
  JuceHttpTransport.{h,cpp}   juce::URL implementation (streams large bodies to a file)
  SecureStore.h               SecureStore interface + MemorySecureStore
  KeychainStore.{h,cpp}       macOS Keychain (Security.framework C API); makePlatformSecureStore()
  Pkce.{h,cpp}                verifier / challenge / state / query parsing (pure functions)
  Tone3000Auth.{h,cpp}        authorize URL, callback handling, token exchange/refresh/persistence
  Tone3000Types.{h,cpp}       UserRef, ToneSummary, Tone, ModelInfo, Page, ClientError; JSON in/out
  Tone3000Client.{h,cpp}      typed REST calls (blocking, worker threads)
  ProviderIndex.{h,cpp}       provenance by sha256 (index.json), image file naming
  DownloadManager.{h,cpp}     job queue on a ThreadPool; fetch, validate, hash, image; outcomes to message thread
  Tone3000Service.{h,cpp}     façade the Engine calls; status, flows, events, placement into the library
engine/bridge/CommandsOnline.cpp   Engine::tone3000* handlers
engine/bridge/Bridge.{h,cpp}       deferred replies; tone3000* dispatch; json::report fetchable; json::asset source
engine/Engine.{h,cpp}              Options (transport, store, client id), hooks, wiring, getState, artFor, assetRefFor
engine/storage/Settings.{h,cpp}    tone3000AutoFetch, tone3000ClientId
engine/storage/RigLoader.{h,cpp}   LoadReport::fetchable
engine/CMakeLists.txt              sources, AMP_TONE3000_CLIENT_ID, Security framework
app/Tone3000AuthWindow.{h,cpp}     in-app web view window intercepting the redirect
app/MainWindow.cpp                 hooks + openExternal native function
ui/src/online/tone3000.ts          TS types, license labels, job registry (events → store)
ui/src/views/tone3000.ts           tab content: splash / connected lists; tone detail modal
ui/src/views/library.ts            TONE3000 tab; attribution on Models/IRs rows
ui/src/views/edit.ts               node menu items, T3K badge, card attribution, missing-node actions
ui/src/views/settings.ts           TONE3000 section
ui/src/main.ts                     "missing from TONE3000" banner
ui/src/state.ts                    tone3000 status, report.fetchable
ui/src/art/archetypes.ts           providerImage through artUrl
ui/src/brand/tone3000/             logo.svg, mark.svg (+ THIRD_PARTY notice)
tests/support/FakeHttp.{h,cpp}     scripted HttpTransport
tests/fixtures/tone3000/*.json     API response fixtures
tests/online_*_test.cpp, tests/bridge_online_test.cpp, tests/nam_a2_test.cpp
docs/bridge-protocol.md, README.md, THIRD_PARTY.md, docs/manual-test-checklist.md
```

---

### Task 1: A2 models load (de-risk before anything else)

**Files:**
- Modify: `tests/CMakeLists.txt` (add `nam_a2_test.cpp`; define `AMP_NAM_EXAMPLES_DIR="${neuralampmodelercore_SOURCE_DIR}/example_models"`)
- Test: `tests/nam_a2_test.cpp`

**Interfaces:**
- Consumes: `amp::NamAmpNode::loadModel(const juce::File&, juce::String& error)`, `amp::makeNamAmp()`, `OfflineRender` (see `tests/nam_amp_test.cpp` for the pattern).
- Produces: nothing; a green test is the deliverable.

- [ ] **Step 1: Write the failing test** `tests/nam_a2_test.cpp`:
  - `TEST_CASE("nam: A2 slimmable container model loads and renders")` — `loadModel(<examples>/A2.nam)` returns non-null (INFO the error); render a 440 Hz, -20 dBFS, 0.5 s sine at 48000/128 through the node; assert every sample finite and the RMS of the second half > -60 dB.
  - `TEST_CASE("nam: A2 feature-test wavenet loads")` — same for `wavenet_a2_feature_test.nam`.
- [ ] **Step 2: Run** `cmake --build build --target amp_tests && ctest --test-dir build -C Debug -R "nam: A2"` → FAIL to configure/compile (missing define) or FAIL on load.
- [ ] **Step 3: Implement** the CMake define. If the load fails, stop: report the NAM core error and the `version` of the file in the hand-over; the spec's fallback (`architecture=1` first) applies to Task 6's model ordering. Do not patch the NAM core.
- [ ] **Step 4: Run** the two tests → PASS.
- [ ] **Step 5: Commit** `test(nam): prove the vendored core loads A2 models`

---

### Task 2: HTTP transport interface, PKCE helpers, fake transport

**Files:**
- Create: `engine/online/HttpTransport.h`, `engine/online/Pkce.{h,cpp}`, `tests/support/FakeHttp.{h,cpp}`
- Modify: `engine/CMakeLists.txt`, `tests/CMakeLists.txt`
- Test: `tests/online_pkce_test.cpp`

**Interfaces:**
- Produces:
  ```cpp
  namespace amp::online {
  struct HttpRequest {
      juce::String method { "GET" }, url, body;        // body sent as application/x-www-form-urlencoded unless a Content-Type header is given
      juce::StringPairArray headers;
      juce::File toFile;                                // non-empty: the body is streamed to this file, response.body stays empty
      int timeoutMs = 30000;
      std::function<void(juce::int64 received, juce::int64 total)> onProgress; // total = -1 when unknown
      std::function<bool()> shouldCancel;               // polled between chunks
  };
  struct HttpResponse {
      int status = 0;                                   // 0 = no HTTP response (see error)
      juce::StringPairArray headers;                    // keys as sent; lookups are case-insensitive via header()
      juce::String body, error;                         // error: transport failure or "canceled"
      bool ok() const { return error.isEmpty() && status >= 200 && status < 300; }
      juce::String header(const juce::String& name) const;
  };
  class HttpTransport { public: virtual ~HttpTransport() = default; virtual HttpResponse request(const HttpRequest&) = 0; /* blocking, any thread */ };

  namespace pkce {
  juce::String randomUrlSafe(int numBytes);                 // base64url (no padding) of random bytes; 32 bytes -> 43 chars
  juce::String challengeFor(const juce::String& verifier);  // base64url(SHA-256(verifier)), RFC 7636
  juce::String encodeQuery(const juce::StringPairArray&);   // k=v&k2=v2, percent-encoded (juce::URL::addEscapeChars, '+' and '/' included)
  juce::StringPairArray parseQuery(const juce::String& queryOrUrl); // text after '?' (or the whole string if none); decoded; last value wins
  }}
  ```
  `amp_test::FakeHttp : HttpTransport` — `void on(const juce::String& method, const juce::String& urlPrefix, std::function<HttpResponse(const HttpRequest&)>)` (first registered prefix match wins), `std::vector<HttpRequest> requests` (every request, in order), `static HttpResponse json(int status, const juce::String& body, juce::StringPairArray headers = {})`, `static HttpResponse file(int status, const juce::MemoryBlock& bytes)` (writes to `toFile` when set). Unmatched request → `{status: 0, error: "no fake response for <method> <url>"}`.
- [ ] **Step 1: Write failing tests** `tests/online_pkce_test.cpp`: `challengeFor` matches RFC 7636 appendix B (`dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk` → `E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM`); `randomUrlSafe(32)` is 43 chars of `[A-Za-z0-9_-]` and two calls differ; `parseQuery("?code=a%20b&state=x&canceled=true")` gives three decoded entries; `parseQuery` on a full URL ignores everything before `?`; `encodeQuery` round-trips through `parseQuery` for a value with `& = / +`; `FakeHttp` prefix matching and the unmatched error.
- [ ] **Step 2: Run** `ctest -R online_pkce` → FAIL (no such target/symbols).
- [ ] **Step 3: Implement** `Pkce.cpp` (`juce::SHA256(const void*, size_t).getRawData()` → `MemoryBlock::toBase64Encoding` then `+`→`-`, `/`→`_`, strip `=`; `juce::Random::getSystemRandom()` for bytes), `FakeHttp.cpp`; add the files to both CMake lists.
- [ ] **Step 4: Run** → PASS.
- [ ] **Step 5: Commit** `feat(online): HTTP transport interface, PKCE helpers and a scripted fake transport`

---

### Task 3: Secure token store

**Files:**
- Create: `engine/online/SecureStore.h`, `engine/online/KeychainStore.{h,cpp}`
- Modify: `engine/CMakeLists.txt` (sources; `if(APPLE) target_link_libraries(amp_engine PRIVATE "-framework Security")`)
- Test: `tests/online_secure_store_test.cpp`

**Interfaces:**
- Produces:
  ```cpp
  namespace amp::online {
  class SecureStore { public: virtual ~SecureStore() = default;
      virtual bool set(const juce::String& key, const juce::String& value) = 0;
      virtual std::optional<juce::String> get(const juce::String& key) = 0;
      virtual bool remove(const juce::String& key) = 0; };                      // thread-safe implementations
  class MemorySecureStore final : public SecureStore { /* std::map under a CriticalSection */ };
  class KeychainStore final : public SecureStore { public: explicit KeychainStore(juce::String service); /* kSecClassGenericPassword, kSecAttrService = service, kSecAttrAccount = key */ };
  std::unique_ptr<SecureStore> makePlatformSecureStore(const juce::String& service); // macOS: KeychainStore; elsewhere: MemorySecureStore
  }
  ```
- [ ] **Step 1: Write failing tests**: `MemorySecureStore` set/get/remove/get-after-remove; a `KeychainStore` round trip under service `com.amperator.tests.<uuid>` that is skipped (`SKIP()`) when `set` returns false (CI without a Keychain), and always removes its item at the end.
- [ ] **Step 2: Run** `ctest -R online_secure_store` → FAIL.
- [ ] **Step 3: Implement** both stores (Keychain: `SecItemCopyMatching` / `SecItemAdd` / `SecItemUpdate` / `SecItemDelete`, UTF-8 data, `kSecAttrAccessibleAfterFirstUnlock`).
- [ ] **Step 4: Run** → PASS (Keychain test passes or is skipped, never fails).
- [ ] **Step 5: Commit** `feat(online): secure store with macOS Keychain and in-memory implementations`

---

### Task 4: Settings fields and the compiled-in client id

**Files:**
- Modify: `engine/storage/Settings.{h,cpp}`, `engine/Engine.h` (`SettingsPatch`), `engine/bridge/CommandsSystem.cpp` (`setSettings`), `engine/bridge/Bridge.cpp` (`settingsPatch`, `json::settings`), `engine/CMakeLists.txt`, `tests/settings_test.cpp`, `tests/bridge_test.cpp`

**Interfaces:**
- Produces: `Settings::tone3000AutoFetch` (bool, default false), `Settings::tone3000ClientId` (juce::String, default empty); `SettingsPatch::tone3000AutoFetch` (`std::optional<bool>`), `SettingsPatch::tone3000ClientId` (`std::optional<juce::String>`); bridge Settings JSON carries both; CMake `set(AMP_TONE3000_CLIENT_ID "" CACHE STRING "TONE3000 publishable key (OAuth client id)")` → `target_compile_definitions(amp_engine PUBLIC AMP_TONE3000_CLIENT_ID="${AMP_TONE3000_CLIENT_ID}")`; `juce::String amp::compiledTone3000ClientId()` in `Engine.cpp`.
- [ ] **Step 1: Write failing tests**: settings round trip of both fields and their defaults (`settings_test.cpp`); bridge `setSettings{tone3000AutoFetch:true, tone3000ClientId:"t3k_pub_x"}` reply echoes them and `getSettings` after a restart (`f.start()`) still has them.
- [ ] **Step 2: Run** `ctest -R "settings|bridge"` → FAIL.
- [ ] **Step 3: Implement** following the `watchedFolders` pattern in `Settings.cpp` and `settingsPatch`.
- [ ] **Step 4: Run** → PASS.
- [ ] **Step 5: Commit** `feat(settings): tone3000AutoFetch, tone3000ClientId and the AMP_TONE3000_CLIENT_ID build variable`

---

### Task 5: Tone3000Auth (authorize URL, callback, token exchange and refresh)

**Files:**
- Create: `engine/online/Tone3000Auth.{h,cpp}`
- Test: `tests/online_auth_test.cpp`

**Interfaces:**
- Consumes: `HttpTransport`, `SecureStore`, `pkce::*` (Tasks 2–3).
- Produces:
  ```cpp
  namespace amp::online {
  struct AuthConfig { juce::String clientId, baseUrl { "https://www.tone3000.com" }, redirectUri { "http://127.0.0.1/callback" }; };
  struct Tokens { juce::String access, refresh; juce::int64 expiresAtMs = 0; };  // stored under key "tokens" as JSON {access, refresh, expiresAtMs}
  struct AuthStart { juce::String url, state; };
  struct CallbackResult { bool stateOk = true, canceled = false, hadCode = false, signedIn = false; juce::String toneId, error; };
  class Tone3000Auth {
  public:
      Tone3000Auth(AuthConfig, HttpTransport&, SecureStore&, std::function<juce::int64()> nowMs = {} /* default Time::currentTimeMillis */);
      AuthStart begin(const juce::StringPairArray& extraParams = {});   // message thread. URL = <base>/api/v1/oauth/authorize?response_type=code&client_id&redirect_uri&scope=read&code_challenge&code_challenge_method=S256&state + extraParams. Remembers state + verifier (one pending flow; a new begin() replaces it).
      CallbackResult complete(const juce::String& query);               // blocking (worker). Validates state, reads tone_id / canceled / error, exchanges `code` (POST /api/v1/oauth/token grant_type=authorization_code&code&client_id&code_verifier&redirect_uri), stores Tokens.
      bool isConnected() const;                                          // tokens present in the store
      juce::String accessToken(juce::String& error);                     // blocking (worker). Refreshes (grant_type=refresh_token&refresh_token&client_id) when expiresAtMs - now < 60000; single-flight under a CriticalSection; refresh 400/401 or body error "invalid_grant" -> clear() and error "signed out"; other failures keep tokens, return "" + message.
      void clear();                                                      // removes "tokens"; fires onSignedOut if it was connected
      std::function<void()> onSignedOut;                                 // any thread
  };
  }
  ```
- [ ] **Step 1: Write failing tests** (FakeHttp + MemorySecureStore + a fake clock):
  - `begin builds the authorize url` — contains every parameter above, `code_challenge == challengeFor(verifier)` (expose the verifier through the exchange request's body in `FakeHttp.requests`), extra params appended.
  - `callback with wrong state is rejected` — `stateOk == false`, no token request made.
  - `callback parameters are decoded` — query `tone_id=t%5F1&state=<st>&code=c%2Bd` → `toneId == "t_1"`, exchange body contains `code=c%2Bd`.
  - `canceled callback with a code still signs in` — `canceled=true&code=x&state=<st>` → `canceled && signedIn`.
  - `exchange stores tokens with expiry` — token JSON `{access_token, refresh_token, token_type, expires_in: 3600}` → store has `expiresAtMs == now + 3600000`.
  - `accessToken refreshes when under 60 s remain` — clock at expiry-59 s → one POST with `grant_type=refresh_token`, new tokens stored, rotation kept.
  - `refresh is single-flight` — two threads call `accessToken` while the fake delays 100 ms → exactly one refresh request.
  - `invalid_grant signs out` — refresh answers 400 `{"error":"invalid_grant"}` → `isConnected() == false`, `onSignedOut` fired once, error `"signed out"`.
  - `network failure keeps tokens` — transport error → tokens still stored, error message returned.
- [ ] **Step 2: Run** `ctest -R online_auth` → FAIL.
- [ ] **Step 3: Implement** `Tone3000Auth.cpp` (JSON via `juce::JSON`; form bodies via `pkce::encodeQuery`; `Content-Type: application/x-www-form-urlencoded`).
- [ ] **Step 4: Run** → PASS.
- [ ] **Step 5: Commit** `feat(online): TONE3000 OAuth PKCE auth with token persistence and single-flight refresh`

---

### Task 6: Types and the typed client

**Files:**
- Create: `engine/online/Tone3000Types.{h,cpp}`, `engine/online/Tone3000Client.{h,cpp}`, `tests/fixtures/tone3000/{user,tone,models_a2,models_a1,models_ir,list_favorited,list_trending,model}.json`
- Test: `tests/online_client_test.cpp`

**Interfaces:**
- Consumes: `Tone3000Auth::accessToken`, `HttpTransport`.
- Produces:
  ```cpp
  namespace amp::online {
  struct UserRef { juce::String id, username, displayName, avatarUrl, url; };
  struct ToneSummary { juce::String id, title, gear, format, license, image, url; UserRef creator; int modelsCount = 0, downloadsCount = 0, favoritesCount = 0; };
  struct ModelInfo { juce::String id, toneId, name, size, architecture /* "1"|"2"|"custom"|"" */, modelUrl; };
  struct Tone : ToneSummary { juce::String description, publishedAt; juce::StringArray makes, tags, images; std::vector<ModelInfo> models; };
  struct Page { std::vector<ToneSummary> tones; int page = 1, totalPages = 1; };
  struct ClientError { enum class Kind { None, NotConfigured, NotConnected, Network, RateLimited, NotFound, Forbidden, Http, Parse } kind = Kind::None; juce::String message; int retryAfterSec = 0; explicit operator bool() const { return kind != Kind::None; } };
  template <class T> struct Result { std::optional<T> value; ClientError error; };
  juce::var toVar(const UserRef&); juce::var toVar(const ToneSummary&); juce::var toVar(const Tone&);   // the bridge shapes of spec §7.4 (camelCase keys; Tone.models as [{id, name, size, architecture}])
  ToneSummary parseToneSummary(const juce::var&); Tone parseTone(const juce::var&); ModelInfo parseModel(const juce::var&); UserRef parseUser(const juce::var&);
  class Tone3000Client {
  public:
      Tone3000Client(Tone3000Auth&, HttpTransport&, juce::String baseUrl);
      Result<UserRef> user();                                        // GET /api/v1/user
      Result<Page> list(const juce::String& kind, int page);         // favorited|downloaded|created: GET /api/v1/tones/<kind>?page=<n>&page_size=25 ; trending|latest: GET /api/v1/tones/<kind> ({data}) -> page 1 of 1
      Result<Tone> tone(const juce::String& id);                     // GET /api/v1/tones/<id> then GET /api/v1/models?tone_id=<id>&architecture=2&page_size=300 and the same with architecture=1; models = A2 list then A1 list; "custom" dropped
      Result<ModelInfo> model(const juce::String& id);               // GET /api/v1/models/<id>
      ClientError fetch(const juce::String& url, const juce::File& to, std::function<void(juce::int64, juce::int64)> progress, std::function<bool()> cancel); // GET with Bearer, streamed
      std::function<void(const juce::String&)> onDeprecation;        // X-Tone3000-Deprecations header value, any thread
  };
  }
  ```
  Every call: `Authorization: Bearer <token>` (NotConnected when the auth has no token), `Accept: application/json`; on 401 one `accessToken` + retry; 429 → RateLimited with `Retry-After`; 404 → NotFound; 403 → Forbidden; other non-2xx → Http with status in the message; unparsable JSON → Parse.
- [ ] **Step 1: Write the fixtures** from the shapes in spec §2/§7.4 (snake_case as TONE3000 sends: `user`, `display_name`, `avatar_url`, `models_count`, `model_url`, `architecture_version`, paginated `{data, page, page_size, total, total_pages}`).
- [ ] **Step 2: Write failing tests**: parse each fixture into the structs (field by field, including `images[0]` → `image`, missing `display_name` → `""`, `architecture_version: null` → `""`); `toVar(Tone)` keys match spec §7.4; `list("favorited", 2)` requests `page=2&page_size=25` with the Bearer header; `list("trending", 1)` replies page 1/1; `tone()` makes three requests in order and orders models A2 then A1 and drops `custom`; 401 then 200 → one refresh and one retry; 429 with `Retry-After: 7` → `RateLimited`, `retryAfterSec == 7`, no second request; deprecation header fires `onDeprecation`; `fetch` streams to the file and passes the Bearer header.
- [ ] **Step 3: Run** `ctest -R online_client` → FAIL.
- [ ] **Step 4: Implement** types and client.
- [ ] **Step 5: Run** → PASS.
- [ ] **Step 6: Commit** `feat(online): TONE3000 typed client over the auth and transport`

---

### Task 7: Provenance index

**Files:**
- Create: `engine/online/ProviderIndex.{h,cpp}`
- Test: `tests/online_provider_index_test.cpp`

**Interfaces:**
- Consumes: `writeFileAtomically` (`engine/storage/AtomicFile.h`), `UserRef`.
- Produces:
  ```cpp
  namespace amp::online {
  struct Provenance { juce::String provider { "tone3000" }, modelId, toneId, title, modelName, size, architecture, format, gear, license, toneUrl, image /* path relative to Art/, e.g. "tone3000/t_456.jpg", or "" */, downloadedAt /* ISO 8601 UTC */; UserRef creator; };
  juce::var toVar(const Provenance&);  // bridge `source`: {provider, id: modelId, toneId, title, creator: toVar(UserRef), license, url: toneUrl, image}
  class ProviderIndex {
  public:
      explicit ProviderIndex(juce::File indexFile);   // <dataDir>/tone3000/index.json
      void load();                                    // missing/corrupt -> empty (corrupt file renamed index.json.corrupt)
      bool save() const;                              // atomic
      void put(const juce::String& sha256, Provenance);
      std::optional<Provenance> bySha(const juce::String& sha256) const;
      std::optional<juce::String> shaForModel(const juce::String& modelId) const;
      size_t size() const;
  };
  }
  ```
- [ ] **Step 1: Write failing tests**: put/bySha/shaForModel; save then load in a new instance round-trips every field (spec §7.2 JSON keys: `provider, modelId, toneId, title, modelName, size, architecture, format, gear, license, creator{username, displayName, avatarUrl, url}, toneUrl, image, downloadedAt`); corrupt file → empty index and `.corrupt` sibling; lowercase sha keys.
- [ ] **Step 2: Run** `ctest -R online_provider_index` → FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** → PASS.
- [ ] **Step 5: Commit** `feat(online): provenance index for downloaded TONE3000 files`

---

### Task 8: File placement and validation helpers

**Files:**
- Create: `engine/online/Placement.{h,cpp}`
- Test: `tests/online_placement_test.cpp`

**Interfaces:**
- Consumes: `RigStore::sanitizeFileName`, `AssetIndex::sha256Of`.
- Produces:
  ```cpp
  namespace amp::online {
  juce::String validateAssetFile(const juce::File&, const juce::String& format);  // "" when ok; format "nam": parses as JSON object with a string "version"; "ir": juce::WavAudioFormat / AiffAudioFormat reader opens with >= 1 channel; else "not a valid <format> file"
  juce::File placementFor(const juce::File& libraryRoot, const Provenance&);     // <root>/<Models|IRs>/TONE3000/<creator>/<title> — <modelName>.<nam|wav>; sanitized; each component trimmed to 80 chars; empty creator -> "unknown"
  juce::File placeFile(const juce::File& partial, const juce::File& target, const juce::String& sha256, juce::String& error); // creates dirs; if target exists with the same sha returns it (partial deleted); else appends " (2)", " (3)"...; moves (copy + delete across volumes)
  }
  ```
- [ ] **Step 1: Write failing tests**: `placement sanitizes creator and title` (creator `"AC/DC: live?"`, title 200 chars with emoji, model `"Standard"` → path inside `Models/TONE3000/`, no extra directory levels, basename ≤ 90 chars, ends with `.nam`); `placeFile` reuses an identical existing file and numbers a different one; `validateAssetFile` accepts `tests/fixtures/assets/tiny.nam` and `ir_48k.wav`, rejects an HTML file as `nam` and a `.nam` file as `ir`.
- [ ] **Step 2: Run** `ctest -R online_placement` → FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** → PASS.
- [ ] **Step 5: Commit** `feat(online): library placement and content validation for downloaded files`

---

### Task 9: DownloadManager

**Files:**
- Create: `engine/online/DownloadManager.{h,cpp}`
- Test: `tests/online_download_test.cpp`

**Interfaces:**
- Consumes: `Tone3000Client::model/tone/fetch`, `validateAssetFile`, `AssetIndex::sha256Of`.
- Produces:
  ```cpp
  namespace amp::online {
  struct DownloadRequest { juce::String jobId, modelId, toneId /* optional */, nodeId /* optional */; juce::var addNode /* {type, index} or void */; juce::String expectedSha /* optional, from the rig */; };
  enum class JobState { Queued, Downloading, Verifying, Done, Failed };
  struct JobEvent { juce::String jobId; JobState state; double progress = 0; juce::String error; /* Done only: */ juce::File file; juce::String sha256; ModelInfo model; Tone tone; juce::File image; /* downloaded tone image or empty */ DownloadRequest request; };
  juce::var toVar(const JobEvent&);  // {jobId, modelId, toneId, nodeId?, state: "queued"|"downloading"|"verifying"|"done"|"failed", progress, file?, sha256?, error?} — the service adds `updated` on done (Task 13)
  class DownloadManager {
  public:
      DownloadManager(Tone3000Client&, juce::File partialDir, juce::File artDir /* <libraryRoot>/Art */);
      std::function<void(const JobEvent&)> onEvent;   // always on the message thread
      juce::String start(DownloadRequest);            // returns jobId (assigned when empty); Queued event at once
      void cancel(const juce::String& jobId);
      void cancelAll();                               // blocking until workers stop; used by the destructor
      static void cleanPartials(const juce::File& partialDir);
  };
  }
  ```
  Job body (worker): `model(modelId)` → `tone(model.toneId)` (skipped when the request's `toneId` is set *and* the caller already passed the tone — the service caches the last `Tone` it showed; otherwise fetched) → `fetch(model.modelUrl, partial)` with progress events at most every 250 ms → `validateAssetFile(partial, tone.format)` → sha256 → image: `tone.images[0]` (if any) fetched to `<artDir>/tone3000/<toneId>.<ext from the URL path, else jpg>` when that file does not exist yet (failure ignored) → Done. Any error → Failed with the `ClientError.message` (`NotFound`/`Forbidden` → `"gone"`), partial deleted.
- [ ] **Step 1: Write failing tests** (FakeHttp with fixtures from Task 6 and `tiny.nam` bytes; pump the message loop like `Fixture::rescan()` does): happy path emits Queued → Downloading(…) → Verifying → Done with `sha256 == AssetIndex::sha256Of(file)` and the image saved; `html body is not accepted as a model` (model_url answers 200 `<html>…` → Failed `"not a valid nam file"`, partial gone); 404 on `/models/x` → Failed `"gone"`; cancel during a delayed fetch → Failed `"canceled"`, partial gone; `cleanPartials` removes leftovers; events arrive on the message thread (`MessageManager::existsAndIsCurrentThread()` asserted inside `onEvent`).
- [ ] **Step 2: Run** `ctest -R online_download` → FAIL.
- [ ] **Step 3: Implement** on a `juce::ThreadPool(2)`; `juce::ThreadPoolJob::shouldExit()` + a per-job cancel flag feed `shouldCancel`.
- [ ] **Step 4: Run** → PASS.
- [ ] **Step 5: Commit** `feat(online): download jobs with validation, hashing and image caching on a thread pool`

---

### Task 10: Deferred replies in the Bridge

**Files:**
- Modify: `engine/bridge/Bridge.{h,cpp}`, `engine/Engine.h` (`CommandResult`), `tests/bridge_test.cpp` (fixture helper), `docs/bridge-protocol.md` (one paragraph under Envelopes)

**Interfaces:**
- Produces: `CommandResult::deferred()` (a result with `pending = true`); `Bridge::Completion` = `std::function<void(CommandResult)>`; `Bridge::Completion Bridge::deferReply()` — callable only while a handler runs (asserts otherwise), captures the message id; calling it later (message thread) sends the reply exactly once (a second call is ignored). `Bridge::handle` skips its own reply when the handler returned `pending`. Test fixture: `juce::var callLater(const juce::String& cmd, const juce::var& args = obj(), int timeoutMs = 5000)` pumps `runDispatchLoopUntil(10)` until the reply with that id arrives.
- [ ] **Step 1: Add the fixture helper** `callLater` to `tests/bridge_test.cpp`'s `Fixture` (no production command defers yet; the behaviour is pinned by Task 11's `tone3000Connect` test, whose reply must arrive only after the window callback, and by `callLater` timing out when a deferred completion is never called).
- [ ] **Step 2: Implement** `CommandResult::pending`, `CommandResult::deferred()`, `Bridge::deferReply()` and the skip in `Bridge::handle`; keep the exception paths unchanged.
- [ ] **Step 3: Run** `ctest -R bridge` → all existing tests still PASS (nothing defers yet).
- [ ] **Step 4: Document** in `docs/bridge-protocol.md`: "Some commands reply later: the reply still carries the request's id; the UI passes a longer timeout to `call()` for them (noted per command)."
- [ ] **Step 5: Commit** `feat(bridge): deferred replies for long-running commands`

---

### Task 11: Tone3000Service and the connect / disconnect / status commands

**Files:**
- Create: `engine/online/Tone3000Service.{h,cpp}`, `engine/bridge/CommandsOnline.cpp`
- Modify: `engine/Engine.{h,cpp}` (Options, hooks, member, getState), `engine/bridge/Bridge.cpp` (dispatch), `engine/CMakeLists.txt`
- Test: `tests/bridge_online_test.cpp` (new fixture `OnlineFixture` = `Fixture` + `FakeHttp* http`, `MemorySecureStore* store`, `o.tone3000ClientId = "t3k_pub_test"`, and a fake auth window that records the URL and lets the test deliver a callback query or a close)

**Interfaces:**
- Consumes: everything above.
- Produces:
  ```cpp
  // Engine::Options additions
  std::unique_ptr<online::HttpTransport> httpTransport;   // null = JuceHttpTransport (Task 14)
  std::unique_ptr<online::SecureStore> secureStore;       // null = makePlatformSecureStore("com.amperator.tone3000")
  juce::String tone3000ClientId;                          // empty = settings.tone3000ClientId, else compiledTone3000ClientId()
  // Engine hooks (set by the app; null = "not available")
  std::function<void(const juce::String& url, const juce::String& redirectPrefix, std::function<void(const juce::String& query)> done)> onOpenAuthWindow; // done("") = closed by the user
  std::function<void()> onClearAuthCookies;
  // Engine commands (CommandsOnline.cpp)
  CommandResult tone3000Status();        // Tone3000Status
  CommandResult tone3000Connect();       // deferred: {connected, user} | {canceled: true}
  CommandResult tone3000Disconnect();    // {}
  // Tone3000Service
  namespace amp::online {
  struct Tone3000Status { bool configured = false, connected = false, busy = false, remembered = true; std::optional<UserRef> user; juce::var toVar() const; }; // remembered = false when the platform store refused writes and tokens live only in memory for this session (spec §11)
  class FallbackSecureStore final : public SecureStore { public: explicit FallbackSecureStore(std::unique_ptr<SecureStore> primary); bool remembered() const; /* set(): primary first; on failure MemorySecureStore from then on and remembered() = false; get()/remove() consult both */ };
  class Tone3000Service {
  public:
      struct Deps { juce::String clientId; HttpTransport& http; SecureStore& store; juce::File dataDir, libraryRoot; AssetIndex& assets; ProviderIndex& index; };
      explicit Tone3000Service(Deps);
      std::function<void(const juce::String& name, const juce::var& data)> emit;             // bridge events
      std::function<void(const juce::String& url, const juce::String& redirectPrefix, std::function<void(const juce::String&)> done)> openAuthWindow;
      std::function<void()> clearCookies;
      Tone3000Status status() const;
      void connect(std::function<void(CommandResult)> reply);     // begin() + window; on callback: complete() on the pool, then user(), emit tone3000Status, reply
      void disconnect();                                           // cancelAll, auth.clear(), clearCookies(), emit tone3000Status
      /* Task 12 adds browse/list/tone; Task 13 adds download/fetch/cancel */
  };
  }
  ```
  `getState` result gains `tone3000: Tone3000Status`. `tone3000Status` event on every change. Not configured → `tone3000Connect` replies `ok:false "not configured"`; no window hook → `ok:false "not available"`; a window already open → `ok:false "busy"`.
- [ ] **Step 1: Write failing tests** in `bridge_online_test.cpp`: `getState` has `tone3000.configured == true/connected == false`; `tone3000Connect` opens the window with a URL containing `prompt`-less authorize params and the state; delivering `code=c&state=<state>` → reply `{connected: true, user.username == "jane"}`, `tone3000Status` event, store has tokens; closing the window (`done("")`) → `{canceled: true}`; wrong state → `ok:false` containing `"didn't match"`; second `tone3000Connect` while open → `ok:false "busy"`; `tone3000Disconnect` → store empty, status event `connected:false`, `clearCookies` called; engine built with an empty client id → `"not configured"`; a primary store whose `set` returns false (test double) still connects and reports `remembered:false`; a restart (`f.start()`) with tokens still in the store → `getState.tone3000.connected == true` and `user` filled after the loop pumps.
- [ ] **Step 2: Run** `ctest -R bridge_online` → FAIL.
- [ ] **Step 3: Implement** service, handlers, dispatch entries, Engine wiring (service constructed after `assets`; `ProviderIndex` loaded from `<dataDir>/tone3000/index.json`; `DownloadManager::cleanPartials` at start).
- [ ] **Step 4: Run** → PASS; full `ctest` still green.
- [ ] **Step 5: Commit** `feat(online): Tone3000Service with connect, disconnect and status over the bridge`

---

### Task 12: Browse, lists and tone detail commands

**Files:**
- Modify: `engine/online/Tone3000Service.{h,cpp}`, `engine/bridge/CommandsOnline.cpp`, `engine/bridge/Bridge.cpp`, `tests/bridge_online_test.cpp`

**Interfaces:**
- Produces:
  ```cpp
  CommandResult Engine::tone3000Browse(const juce::String& format, const juce::String& nodeId, const juce::String& loadTone); // deferred: {toneId} | {canceled: true}
  CommandResult Engine::tone3000List(const juce::String& kind, int page);   // deferred: {tones: [ToneSummary], page, totalPages}
  CommandResult Engine::tone3000Tone(const juce::String& id);               // deferred: Tone
  ```
  Browse: when not connected the same window runs the plain connect flow first (one window: the authorize URL without `prompt`, then on success a second load with `prompt=select_tone`); extra params `prompt=select_tone&preview=true&menubar=true&architecture=2` plus `format=<nam|ir>` when given; `loadTone` non-empty → `prompt=load_tone&tone_id=<loadTone>` instead. The callback's `tone_id` is the reply; `canceled=true` → `{canceled: true}`; a `code` present is exchanged (keeps the session fresh). `nodeId` is echoed in the reply as `nodeId` so the UI can route the result. Argument validation: `format` ∈ {`nam`, `ir`, absent}; `kind` ∈ the five list names; `page` ≥ 1.
- [ ] **Step 1: Write failing tests**: browse URL contains `prompt=select_tone`, `preview=true`, `menubar=true`, `architecture=2`, `format=ir`; callback `tone_id=t_1&state=…` → `{toneId: "t_1", nodeId: "n3"}`; `canceled=true` → `{canceled: true}`; browse while disconnected opens the authorize URL first, then the select URL after a successful code callback (two window openings recorded); `loadTone` → `prompt=load_tone&tone_id=t_9`; `tone3000List{kind:"favorited", page:1}` replies the fixture's two tones with `creator.username`; invalid `kind` → `ok:false`; `tone3000List` while disconnected → `ok:false "not connected"`; `tone3000Tone{id}` replies the fixture tone with models A2-first; a 429 → `ok:false` message containing `"try again in 7 s"`.
- [ ] **Step 2: Run** → FAIL.
- [ ] **Step 3: Implement** (list/tone run the client call on the service's own `juce::ThreadPool(2)`; results posted back and replied through the deferred completion).
- [ ] **Step 4: Run** → PASS.
- [ ] **Step 5: Commit** `feat(online): browse via TONE3000's picker, bounded lists and tone detail commands`

---

### Task 13: Download, placement into the rig, provenance in listAssets, provider art, fetch-missing

**Files:**
- Modify: `engine/online/Tone3000Service.{h,cpp}`, `engine/bridge/CommandsOnline.cpp`, `engine/bridge/Bridge.cpp` (`json::asset` source, `json::report` fetchable, dispatch), `engine/Engine.{h,cpp}` (`assetRefFor`, `artFor`, `installRig` fetchable + auto-fetch, `listAssets`), `engine/storage/RigLoader.{h,cpp}` (`LoadReport::fetchable`), `tests/bridge_online_test.cpp`, `tests/rig_loader_test.cpp`

**Interfaces:**
- Produces:
  ```cpp
  CommandResult Engine::tone3000Download(const juce::String& modelId, const juce::String& toneId, const juce::String& nodeId, const juce::var& addNode); // {jobId}; events tone3000Download
  CommandResult Engine::tone3000Fetch(const juce::StringArray& nodeIds);   // {jobs: [jobId]}; empty list = every fetchable node
  CommandResult Engine::tone3000Cancel(const juce::String& jobId);         // {}
  // Service
  std::function<CommandResult(const juce::File& placed, const juce::String& sha, const online::DownloadRequest&)> placeInRig; // set by Engine: setAsset(nodeId, file) or addNode(type, index, {}, file); fills asset.source {tone3000, modelId}
  ```
  Behaviour on a Done job (message thread): `placeFile` → `ProviderIndex::put(sha, provenance)` + `save()` → `assets.rescan()` → `placeInRig` when the request has a node target → event `tone3000Download{state:"done", file, sha256, updated: <sha != expectedSha when expectedSha given>}`. `json::asset` adds `source` from `ProviderIndex::bySha`. `Engine::assetRefFor` fills `source` from the index. `Engine::artFor` adds `providerImage` = absolute path of `Art/<image>` when the node's asset sha has provenance with an image. `LoadReport::fetchable` (filled in `Engine::installRig`): placeholder node ids whose `asset.source.provider == "tone3000"`; `json::report` writes it. `tone3000Fetch` builds one `DownloadRequest{modelId: source.id, nodeId, expectedSha: asset.sha256}` per node; when not connected it replies `ok:false "not connected"` (the UI connects first). Auto-fetch: after `installRig` of a *loaded* rig (not a structural edit), when `settings.tone3000AutoFetch && status().connected && !fetchable.isEmpty()` → `tone3000Fetch({})`.
- [ ] **Step 1: Write failing tests**: `tone3000Download{modelId, nodeId}` on the default rig's NamAmp → pump → file exists under `Models/TONE3000/jane/…`, `rigLoaded` carries the node with `asset.source == {tone3000, m_123}` and `art.providerImage` ending in `Art/tone3000/t_456.jpg`, `listAssets{nam}` row has `source.creator.username == "jane"` and `source.license == "cc-by"`; `saveRig` then `loadRig` keeps `source`; `addNode:{type:"CabIR", index:2}` places a new node; `duplicate downloads share one file` (two jobs for the same model on two nodes → one file, two Done events, both nodes assigned); rig fixture with a tone3000-sourced missing asset → `rigLoaded.report.fetchable == [n3]`; `tone3000Fetch{}` → node assigned, `updated:false` when the sha matches and `true` when the fake serves different bytes; `fetch while offline fails each job and keeps fetchable` (transport error → `failed` event per job, `getState.tone3000.busy == false`, `report.fetchable` unchanged); 404 → `failed "gone"`, node still a placeholder; `tone3000Cancel`; `setAsset` with a file already in the index fills `source` (`rig_loader`/bridge test); `tone3000AutoFetch:true` + connected → a load starts jobs without a command; not connected → no window opened.
- [ ] **Step 2: Run** → FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** → PASS; full `ctest` green (existing `rigLoaded.report` consumers get the extra key).
- [ ] **Step 5: Commit** `feat(online): downloads into the library and the rig, provenance in assets and art, one-click fetch of missing TONE3000 files`

---

### Task 14: Real HTTP transport

**Files:**
- Create: `engine/online/JuceHttpTransport.{h,cpp}`
- Modify: `engine/CMakeLists.txt`, `engine/Engine.cpp` (default transport), `tests/CMakeLists.txt`
- Test: `tests/online_http_live_test.cpp`

**Interfaces:**
- Produces: `class JuceHttpTransport final : public HttpTransport` — `juce::URL::createInputStream(InputStreamOptions{}.withExtraHeaders(...).withConnectionTimeoutMs(timeoutMs).withResponseHeaders(&h).withStatusCode(&status).withHttpRequestCmd(method))`; POST bodies via `withPOSTData`; `toFile` set → copy the stream to the file in 64 KiB chunks, calling `onProgress`/`shouldCancel` per chunk; `Content-Length` → total.
- [ ] **Step 1: Write the test** `TEST_CASE("live: tone3000 latest without a token answers 401", "[live]")` guarded by `if (juce::SystemStats::getEnvironmentVariable("AMP_NETWORK_TESTS", "") != "1") SKIP("set AMP_NETWORK_TESTS=1");` → GET `https://www.tone3000.com/api/v1/tones/latest` → `status == 401` (or 403), `error.isEmpty()`; plus a streamed GET of `https://www.tone3000.com/robots.txt` to a temp file with `onProgress` called at least once.
- [ ] **Step 2: Run** with the variable set → FAIL (no class).
- [ ] **Step 3: Implement**; make it the Engine's default when `Options::httpTransport` is null.
- [ ] **Step 4: Run** `AMP_NETWORK_TESTS=1 ctest -R live` → PASS; without the variable → skipped.
- [ ] **Step 5: Commit** `feat(online): juce::URL HTTP transport`

---

### Task 15: App window and openExternal

**Files:**
- Create: `app/Tone3000AuthWindow.{h,cpp}`
- Modify: `app/MainWindow.{h,cpp}`, `app/CMakeLists.txt`, `ui/src/bridge.ts` (`openExternal(url)` helper on `invokeNative`), `ui/src/test-support.ts` (`__juce__functions` list)

**Interfaces:**
- Produces: `class Tone3000AuthWindow : juce::DocumentWindow` (title "TONE3000", 980×760, centred on the main window) holding a `juce::WebBrowserComponent` subclass whose `pageAboutToLoad(url)` returns `false` and calls `done(queryOf(url))` when `url` starts with the redirect prefix; `closeButtonPressed` → `done("")`. The app sets `engine.onOpenAuthWindow` (one window at a time; a second request while open is answered by the engine's "busy") and `engine.onClearAuthCookies = [] { juce::WebBrowserComponent::clearCookies(); }`. Native function `openExternal(url)` → `juce::URL(url).launchInDefaultBrowser()` only when `url` starts with `https://www.tone3000.com/` (else completes `false`). UI: `export function openExternal(url: string): Promise<boolean>` in `bridge.ts`.
- [ ] **Step 1: Write the failing UI test** (`bridge.test.ts`): `openExternal('https://www.tone3000.com/x')` sends `__juce__invoke` with `name: 'openExternal'` and resolves with the completion value.
- [ ] **Step 2: Run** `cd ui && npx vitest run src/bridge.test.ts` → FAIL.
- [ ] **Step 3: Implement** the window, the hooks and the native function; build the app.
- [ ] **Step 4: Verify** `cmake --build build --target AMPerator` succeeds; UI test PASS. Manual (if a client id and account are available): `AMP_TONE3000_CLIENT_ID=<key>` build, Connect from the (Task 16) tab, the window closes itself after the code.
- [ ] **Step 5: Commit** `feat(app): in-app TONE3000 sign-in window and openExternal`

---

### Task 16: UI state, TONE3000 tab (splash, lists) and library wiring

**Files:**
- Create: `ui/src/online/tone3000.ts`, `ui/src/views/tone3000.ts`, `ui/src/brand/tone3000/logo.svg`, `ui/src/brand/tone3000/mark.svg`
- Modify: `ui/src/state.ts`, `ui/src/views/library.ts`, `ui/src/styles.css`, `THIRD_PARTY.md`
- Test: `ui/src/views/tone3000.test.ts`

**Interfaces:**
- Produces (`online/tone3000.ts`): `interface Tone3000Status { configured: boolean; connected: boolean; busy: boolean; remembered: boolean; user: UserRef | null }`, `UserRef`, `ToneSummary`, `Tone`, `DownloadJob` (spec §7.4 shapes), `licenseLabel(id: string): string` (`t3k` → "TONE3000 license", `cc-by` → "CC BY — credit the creator", `cc-by-sa` → "CC BY-SA — credit, share alike", `cc-by-nc` → "CC BY-NC — credit the creator, non-commercial", `cc-by-nc-sa` → "CC BY-NC-SA — credit, non-commercial, share alike", `cc-by-nc-nd` → "CC BY-NC-ND — credit, non-commercial, no derivatives", `cc-by-nd` → "CC BY-ND — credit, no derivatives", `cco` → "CC0 — public domain", other → the id), `connectTone3000(): Promise<boolean>` (calls `tone3000Connect` with a 600 000 ms timeout; toasts errors except canceled), `jobs` registry: `onJob(fn)`, `jobsFor(modelId)`, fed by `tone3000Download` events, toasts `done` ("<title> added to your library" / "… was updated on TONE3000 since this rig was saved" when `updated`) and `failed` ("Couldn't download <title>: <error>"; `gone` → "<title> is no longer on TONE3000"). `state.ts`: `AppState.tone3000: Tone3000Status` seeded by `getState.tone3000`, updated by the `tone3000Status` event; `LoadReport.fetchable: string[]`.
  `views/tone3000.ts`: `renderTone3000Tab(content: HTMLElement): void` — not configured → `<p class="muted">This build has no TONE3000 client id.</p>`; not connected → splash (logo, the fixed copy, `Connect TONE3000`, "Powered by TONE3000" link via `openExternal`); connected → header (avatar, username, `Disconnect`), `Browse TONE3000` (+ NAM / IR chips, default none) → `tone3000Browse{format?}` (600 000 ms timeout) → `openToneDetail(toneId)` (Task 17), sub-tabs Favorites · Downloaded · Created · Trending · Latest → `tone3000List{kind, page}` (30 000 ms), rows `li.tone-row` with image (or archetype fallback via `guessArchetype`), title, gear, format badge, creator avatar + username, "T3K" mark when `inLibrary` (a `Set<string>` of model ids from `listAssets` sources, refreshed on tab open and after `done` jobs); pager with "Discover more on TONE3000"; errors in place with Retry. `library.ts`: `Tab` gains `'tone3000'`; the tab button shows the logo image with `aria-label="TONE3000"`; Models / IRs rows render `source.creator.username` · `licenseLabel(source.license)` under the name when `source` is present.
- [ ] **Step 1: Get the logos**: download `https://www.tone3000.com/TONE3000%20Logos.zip` into a fresh scratch directory, pick the full logo and the short mark (SVG preferred, else PNG → `logo.png`/`mark.png`), copy into `ui/src/brand/tone3000/`, add to `THIRD_PARTY.md`: "TONE3000 logo and T3K mark — trademarks of TONE3000, used under its brand guidelines (tone3000.com/api, Design Requirements); not covered by this project's AGPL license." If the download fails, commit a plain-text `TONE3000` wordmark SVG as a stand-in and note it in the hand-over.
- [ ] **Step 2: Write failing tests** `tone3000.test.ts` (pattern: `settings.test.ts`): not configured line; splash shows the fixed copy and `Connect TONE3000` sends `tone3000Connect`; after `getState` with `connected:true` the lists request `tone3000List{kind:"favorited", page:1}` and a reply renders rows with title, creator username and `img[alt]`; `Browse TONE3000` with the IR chip sends `tone3000Browse{format:"ir"}`; a `tone3000Status{connected:false}` event returns to the splash; `licenseLabel` table; a `failed` job event toasts the message.
- [ ] **Step 3: Run** `npx vitest run src/views/tone3000.test.ts` → FAIL.
- [ ] **Step 4: Implement**; `npm run build` passes `tsc`.
- [ ] **Step 5: Run** all UI tests → PASS.
- [ ] **Step 6: Commit** `feat(ui): TONE3000 library tab with splash, bounded lists and attribution rows`

---

### Task 17: Tone detail modal, download progress, add to rig

**Files:**
- Modify: `ui/src/views/tone3000.ts`, `ui/src/online/tone3000.ts`, `ui/src/styles.css`
- Test: `ui/src/views/tone3000.test.ts`

**Interfaces:**
- Produces: `openToneDetail(toneId: string, target?: { nodeId: string } | { addToRig: true }): void` — calls `tone3000Tone{id}` (30 000 ms), renders image, title, gear, format, creator (avatar, username, `View on TONE3000` → `openExternal(creator.url)` / `tone.url`), `licenseLabel`, description, model selector (`<select>` listing `name · size · A2/A1`, A2 first, pre-selected first A2, "in library" suffix for models whose id is in the library set), actions: `Download` (no target) → `tone3000Download{modelId, toneId}`; `Add to rig` → `tone3000Download{modelId, toneId, addNode: {type, index}}` where `type` = `NamAmp` for format `nam` else `CabIR`, and `index` from the current rig per spec §9.4 (NAM: right after the current NamAmp, else before the first CabIR, else at the end; IR: after the last NamAmp/CabIR, else at the end — compute from `store.get().rig.nodes` in signal order, counting non-I/O nodes); `Use on <node title>` when `target.nodeId` → `tone3000Download{modelId, toneId, nodeId}`. Progress bar bound to `jobsFor(modelId)`; closing the modal keeps the job.
- [ ] **Step 1: Write failing tests**: detail renders the fixture tone (title, `jane`, license label, 3 options A2 first, first A2 selected); `Add to rig` on the default rig (In, Gate, NamAmp, CabIR, Out) sends `addNode: {type:"NamAmp", index: 2}` for a `nam` tone and `{type:"CabIR", index: 3}` for an `ir` tone; `Use on` sends `nodeId`; a `downloading` event with `progress 0.5` sets the bar to 50%; `done` closes nothing and the toast appears.
- [ ] **Step 2: Run** → FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** → PASS.
- [ ] **Step 5: Commit** `feat(ui): tone detail with model selection, download progress and add-to-rig`

---

### Task 18: Pedalboard, banner, art tier 2, settings section

**Files:**
- Modify: `ui/src/views/edit.ts`, `ui/src/main.ts`, `ui/src/art/archetypes.ts`, `ui/src/views/settings.ts`, `ui/src/styles.css`
- Test: `ui/src/views/edit.test.ts`, `ui/src/art/archetypes.test.ts`, `ui/src/views/settings.test.ts`, `ui/src/main.test.ts` (create if absent, pattern of the other view tests)

**Interfaces:**
- Consumes: `openToneDetail`, `connectTone3000`, `jobsFor`, `AppState.tone3000`, `report.fetchable`, `node.asset.source`, `node.art.providerImage`.
- Produces: node menu items for NamAmp/CabIR — `Browse TONE3000…` → `tone3000Browse{format, nodeId}` then `openToneDetail(toneId, {nodeId})` (connect first when `!tone3000.connected`); placeholder nodes with `asset.source.provider === 'tone3000'` additionally get `Download from TONE3000` → `tone3000Fetch{nodeIds:[id]}` and `Replace from TONE3000…` → `tone3000Browse{format, nodeId, loadTone: source.toneId ?? ''}`; empty NamAmp/CabIR cards show a second button `Browse TONE3000`; cards with a TONE3000 source show `span.t3k-mark` (mark.svg, `title="From TONE3000"`) and, when expanded, `title · creator.username` with `View on TONE3000`. `main.ts`: banner `${n} model${n===1?'':'s'} from TONE3000 ${n===1?'is':'are'} missing.` with a `Download` button → connect if needed, then `tone3000Fetch{}`; dismissed per rig like the report banner. `archetypes.ts`: `resolveArt` uses `artUrl(art.providerImage)` (tier 2) — update the two existing tests that pass a raw URL to pass an `Art/tone3000/…` path and expect `art/tone3000/…`. `settings.ts`: section "TONE3000" with status line (plus "Sign-in will not be remembered on this Mac." when `remembered === false`), `Connect TONE3000` / `Disconnect`, checkbox "Download missing TONE3000 models automatically when opening a rig" → `setSettings{tone3000AutoFetch}`, and a client-id text field (shown only when `settings.tone3000ClientId` is non-empty or the build is not configured) → `setSettings{tone3000ClientId}`.
- [ ] **Step 1: Write failing tests**: menu shows `Browse TONE3000…` for NamAmp and sends `tone3000Browse{format:"nam", nodeId}`; placeholder with a tone3000 source shows `Download from TONE3000` → `tone3000Fetch{nodeIds:[id]}`; card with source renders `.t3k-mark`; `report.fetchable:['n3']` renders the banner text "1 model from TONE3000 is missing." and `Download` sends `tone3000Fetch{}` when connected; `resolveArt` provider path test; settings checkbox sends `setSettings{tone3000AutoFetch:true}`.
- [ ] **Step 2: Run** `npx vitest run` → FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** → PASS; `npm run build` clean.
- [ ] **Step 5: Commit** `feat(ui): TONE3000 on the pedalboard, missing-model banner, provider art and settings`

---

### Task 19: Documentation and manual checklist

**Files:**
- Modify: `docs/bridge-protocol.md` (new commands/events/shapes from spec §7.4/§8, deferred replies, `art/tone3000/` images, `getState.tone3000`), `README.md` ("Powered by TONE3000" paragraph with link, how to build with `-DAMP_TONE3000_CLIENT_ID=…`, where files land), `THIRD_PARTY.md` (if not done in Task 16), `docs/manual-test-checklist.md` (new section "TONE3000": first sign-in window closes itself and the Keychain prompt appears once; picker shows preview players; an A2 model plays at 48 kHz and 44.1 kHz; an IR tone loads into CabIR; delete the file, reopen the rig, banner → Download restores it; offline launch keeps local playback and lists show Retry; Disconnect then Connect without re-entering the code; `Replace from TONE3000…` on a gone model)

- [ ] **Step 1: Write** the docs; every command in `CommandsOnline.cpp` appears in the protocol table with args, result and events.
- [ ] **Step 2: Verify** `grep -c tone3000 docs/bridge-protocol.md` > 0 for each of: `tone3000Status`, `tone3000Connect`, `tone3000Disconnect`, `tone3000Browse`, `tone3000List`, `tone3000Tone`, `tone3000Download`, `tone3000Fetch`, `tone3000Cancel`.
- [ ] **Step 3: Run** the whole suite one last time: `ctest --test-dir build -C Debug` and `cd ui && npx vitest run` → green; `cmake --build build --target AMPerator` → builds.
- [ ] **Step 4: Commit** `docs: TONE3000 bridge commands, README and manual checklist`
