# AMPerator — Tone Library: TONE3000 in the app (Sub-project 3)

**Date:** 2026-10-20
**Status:** Draft for review
**Builds on:** `2026-10-19-amperator-core-design.md` (sub-project 1, merged)

## 1. Purpose

Today a player who wants a new amp model or cab IR leaves AMPerator, finds the file on a website, saves it, and
drops it into `~/Music/AMPerator/Models` or `IRs`. This sub-project puts TONE3000 (formerly ToneHunt, the main
public library of NAM captures and IRs) inside the app, so that from the pedalboard you can browse TONE3000,
pick a tone, and play it, with nothing to find or move by hand. Rigs remember where each model came from, so a rig
opened on a machine that lacks the file can fetch it again with one click.

**Who it is for:** the same single player as sub-project 1 (Scarlett Solo, 3-pedal footswitch, plays at home).
Sub-project 4 (the AI tone builder) is the second consumer: it needs a way to turn a TONE3000 model id into a
local file and a node, which this sub-project provides.

**Success criteria**
1. From the pedalboard, "Browse TONE3000" on an amp or cab node leads to a playable rig with the chosen
   tone in under a minute, including first-time sign-in, with no file dialogs.
2. A saved rig re-opened on a Mac without the files shows which parts come from TONE3000 and restores them with
   one click (after sign-in), ending with the same sha256 the rig recorded.
3. Every downloaded file keeps its creator, license and TONE3000 link, visible in the library and on the node, and
   the integration follows TONE3000's free-tier API terms and design requirements (§9) with no exceptions.
4. The engine stays headless and testable: every network and sign-in path has a fake, and the audio thread is
   never touched by any of this.

## 2. What the research found (constraints that shape the design)

Verified on 2026-10-20 against tone3000.com/api, tone3000.com/api/terms (effective 2026-07-07), tone3000.com/terms
(effective 2025-03-12), the integration guide at github.com/tone-3000/api (with a JUCE example), and third-party
integrations (Axion, the TONE3000 plugin).

| Fact | Consequence for AMPerator |
|---|---|
| The API (`https://www.tone3000.com/api/v1`) is OAuth 2.0 + PKCE only; every endpoint needs a signed-in TONE3000 user. Sign-in is email + 6-digit code (no password). The publishable key `t3k_pub_…` is the OAuth `client_id` and may ship in client apps; the secret key must not. Loopback redirect URIs (`http://127.0.0.1:<port>/callback`) are always accepted. | The app signs the user in with their own TONE3000 account; AMPerator ships only the publishable key (§12). |
| **Free tier** (non-commercial products such as open-source projects) may use **only** the OAuth prompt flows `select_tone` and `load_tone` plus the bounded list endpoints `favorited`, `downloaded`, `created`, `trending`, `latest`. `/tones/search`, `/makes`, `/tags`, `/users` and favoriting are commercial-tier. No sign-off is needed for free-tier integrations. | AMPerator does **not** build its own search. "Search" is TONE3000's own picker page (`prompt=select_tone`, with preview players and filters), opened from the app and returning a `tone_id`. Our UI draws the five bounded lists. |
| After the picker: `GET /tones/{id}` and `GET /models?tone_id={id}&architecture=…` give the tone's models; each model has a `model_url` fetched with the Bearer token. `GET /models/{id}` works on its own when the parent tone is public. The archive endpoint `/tones/{id}/download` is partners-only. A2 models are returned only when `architecture=2` is passed explicitly. | Downloads are per model file, never archives. Missing-asset fetch needs only the model id. Ask for A2 first, then A1. |
| Terms: download only on a user's request; never bulk download, mirror, cache or preload the catalog; never make content available outside the integration (no re-hosting, no sharing); preserve creator name, metadata and license (licenses: `t3k`, `cc-by`, `cc-by-sa`, `cc-by-nc`, `cc-by-nc-sa`, `cc-by-nc-nd`, `cc-by-nd`, `cco`); present browsing and delivery as TONE3000 under its branding; "Powered by TONE3000" + a link to tone3000.com; delete a user's data on disconnect; access tokens only for the user who authorized them. | Rigs carry a reference (`source`), never the file. Downloads happen on a click. Lists are fetched live and not persisted. Attribution and license are stored next to each file and shown. Disconnect wipes tokens and user info. |
| Design requirements: the entry point sits where a tone is added to a signal block; a partnership splash before sign-in; after sign-in a tone list first (image, title, gear, format, creator username + avatar, signed-in user shown); Favorites / Created / Downloaded tabs recommended, trending and latest for discovery; a persistent "Browse TONE3000" path to the full catalog; a loaded tone shows its image and the T3K mark on its block; a tone detail view with a model selector; full logo before the short T3K mark. Logo files: `tone3000.com/TONE3000 Logos.zip`. | §9 follows these one by one. The core spec's "no brand names or logos" rule is about **built-in archetype art**; provider branding required by the provider's terms is a different thing and is kept to the TONE3000 surfaces. |
| Rate limit 100 requests/min by default; search (not used) is tighter. Tokens: refresh ~60 s before expiry, one refresh at a time, retry once on 401, `invalid_grant` on refresh means sign in again. Refresh tokens rotate (store every new one). Deprecations arrive in an `X-Tone3000-Deprecations` header. | Human-paced use stays far below the limit. The client implements exactly these rules and logs deprecations. |
| The official JUCE example keeps tokens in the macOS Keychain / Windows DPAPI, runs HTTP on a worker thread, and offers either the system browser + loopback server or an in-app `WebBrowserComponent` whose `pageAboutToLoad` intercepts the redirect. | Same choices here (§5, §12). |
| The vendored NeuralAmpModelerCore (commit of 2026-09-18) contains the A2 pieces (FiLM, gating activations, slimmable layers; `NAM_ENABLE_A2_FAST` is set). | A2 files should load; the plan verifies this with a real A2 download before building on it. |

## 3. Approaches considered

**A. Free-tier integration: TONE3000's picker + bounded lists + per-model downloads (recommended, chosen).**
The app opens TONE3000's own `select_tone` page in a window, receives a `tone_id`, shows the tone's models, and
downloads the chosen file with the user's token. Our own UI only draws the user's Favorites / Downloaded / Created
and the Trending / Latest lists. Pros: the only path the terms allow for an open-source app; TONE3000 maintains
the search, filters, previews and sign-in; nothing to approve. Cons: search lives in a TONE3000 page, not in our
look; every list call needs the user signed in; sub-project 4 cannot search by text (see §13).

**B. Full API with our own search UI.** Needs a commercial agreement and TONE3000 sign-off, which an AGPL hobby
app will not get, and the terms forbid presenting the catalog as our own. Rejected.

**C. No API: watch the browser's download folder / "paste a TONE3000 URL".** Works without a key, but there is
no public download endpoint without a token, no attribution, no auto-fetch, and it is the scraping pattern the
terms prohibit. Rejected; the existing watched-folder feature already covers the manual case.

## 4. Scope

**In**
- Sign in to TONE3000 from AMPerator (in-app window), stay signed in across launches, disconnect.
- A **TONE3000** tab in the library panel (replacing the disabled "Search online" tab): splash, the five bounded
  lists, "Browse TONE3000", a tone detail view with model selection, download, and "Add to rig".
- "Browse TONE3000…" on NamAmp and CabIR nodes (edit view node menu and the empty-node card), scoped to the node's
  format (`nam` / `ir`).
- Downloaded files land in the library with attribution (creator, license, tone link, image) kept beside them and
  shown in the Models / IRs tabs, the node card and the tone detail view.
- Rigs record `asset.source = {provider: "tone3000", id: <model id>}`; a missing TONE3000 asset is restored with one
  click from the load banner or the node; "Replace from TONE3000…" for a model that is gone.
- Provider image as node art (tier 2 of the core spec's art order).
- Bridge protocol version stays 1 with additive commands and fields (§8).

**Out (and why)**
- Our own text search, make/tag browsing, favoriting, uploading: commercial-tier endpoints.
- Audition/preview player in the app: TONE3000's picker has preview players (`preview=true`); in the app the
  real audition is playing the rig.
- Any cache of the catalog, "download all my favorites", or bundled models: forbidden by the terms.
- Other providers. The engine's `source.provider` stays generic and the C++ units are provider-shaped, but only
  TONE3000 is built; nothing is abstracted beyond what one provider needs.
- Windows secure storage and WebView2 details: sub-project 5 (the `SecureStore` and auth-window seams are defined
  here so that work is additive).

## 5. Architecture

```
┌──────────── UI (web view) ────────────┐   ┌──────── app (JUCE) ────────┐
│ library TONE3000 tab · node menu      │   │ Tone3000AuthWindow         │
│ load banner "Download from TONE3000"  │   │  (WebBrowserComponent in a │
│ attribution rows · T3K node badge     │   │   DocumentWindow; grabs    │
└──────────▲──────────────┬─────────────┘   │   the redirect)            │
           │ events       │ commands        └──────▲──────────┬──────────┘
┌──────────┴──────────────▼───────────────────────┴──────────▼──────────┐
│ Engine (message thread)                                                 │
│   Tone3000Service  ── owns ──►  Tone3000Auth (PKCE, tokens, SecureStore)│
│        │                        Tone3000Client (REST over HttpTransport)│
│        │                        DownloadManager (worker thread pool)    │
│        │                        ProviderIndex (<dataDir>/tone3000/…)    │
│        ▼                                                                │
│   AssetIndex · RigLoader · RigStore (unchanged except `source` join)    │
└─────────────────────────────────────────────────────────────────────────┘
```

All new engine code lives in `engine/online/`. Nothing in it touches the audio thread; the Engine remains
message-thread only, with network work on worker threads that post results back with `MessageManager::callAsync`.

### 5.1 Units

| Unit | Responsibility | Depends on | Test double |
|---|---|---|---|
| `HttpTransport` (interface) | `request(method, url, headers, body, onProgress) → {status, headers, body or file}`; cancellation; timeouts. `JuceHttpTransport` implements it with `juce::URL` (macOS: NSURLSession; `JUCE_USE_CURL=0` stays). Large bodies stream to a file. | juce_core | `FakeHttpTransport`: scripted responses by URL pattern, records requests, can delay/fail/429. |
| `SecureStore` (interface) | `set(key, bytes)`, `get(key)`, `remove(key)` for the token set. `KeychainStore` (macOS, Security.framework C API, service `com.amperator.tone3000`), later `DpapiStore` (Windows). | OS | `MemoryStore`. |
| `Tone3000Auth` | PKCE verifier/challenge/state, authorize-URL building (`/oauth/authorize`), code exchange and refresh (`/oauth/token`), token persistence, single-flight refresh 60 s before expiry, `invalid_grant` → signed out. Exposes `withToken(fn)` so the client never sees raw tokens. | HttpTransport, SecureStore | FakeHttpTransport + MemoryStore. |
| `Tone3000Client` | Typed calls: `user()`, `list(kind, page, pageSize, format)`, `tone(id)`, `models(toneId, architecture)`, `model(id)`, `fetchModelFile(modelUrl, toFile, onProgress)`, `fetchImage(url, toFile)`. Parses JSON into `ToneSummary`, `Tone`, `Model`, `UserRef`. Retries once on 401 after a refresh; maps 429 (`Retry-After`) and network failures to typed errors; logs `X-Tone3000-Deprecations`. | Tone3000Auth, HttpTransport | FakeHttpTransport. |
| `DownloadManager` | Queue of jobs `{jobId, modelId, toneId, nodeId?, kind}`; runs each on a `juce::ThreadPool` (2 threads): fetch to `<dataDir>/tone3000/.partial/<jobId>`, sha256 on the worker, then hands `{file, sha}` to the message thread, which moves the file into the library (copy + delete when the two are on different volumes). Progress events at most 4 Hz. Cancel on disconnect or quit. | Tone3000Client | Driven with FakeHttpTransport; a job's finish is observable on the message loop. |
| `ProviderIndex` | `<dataDir>/tone3000/index.json`: sha256 → provenance record (§7.2). Atomic writes (`AtomicFile`). Lookup by sha and by model id. Image cache files in `Art/tone3000/`. | filesystem | Temp dir. |
| `Tone3000Service` | The façade the Engine calls: status, connect/disconnect, open picker, lists, tone detail, download, fetch-missing. Owns the units above, turns their results into bridge events, and asks the Engine to place files and assign nodes. | all above, Engine hooks | Whole-service tests through the Bridge with fakes injected via `Engine::Options`. |
| `Tone3000AuthWindow` (app) | A `DocumentWindow` holding a `WebBrowserComponent` subclass whose `pageAboutToLoad` intercepts `http://127.0.0.1/callback?…` (the redirect URI; no server listens, the request never leaves the view). Closes itself after the callback or when the user closes it. One at a time. Its cookie store persists, so later picker openings skip sign-in. | JUCE | The engine's hook `onOpenAuthWindow(url, redirectPrefix, onCallback)` is a `std::function`; tests set a fake that calls back at once. |

### 5.2 Engine hooks and options

- `Engine::Options` gains `std::unique_ptr<HttpTransport> httpTransport` (null = `JuceHttpTransport`),
  `std::unique_ptr<SecureStore> secureStore` (null = the platform store), and `juce::String tone3000ClientId`
  (empty = the compiled-in default, §12).
- `Engine::onOpenAuthWindow` (like `onOpenPluginEditor`): the app opens the window; the engine never owns windows.
- Downloads are cancelled when the quit is confirmed (`onQuitConfirmed` path) and in `~Engine`; partial files in `.partial/` are deleted at the next start.

## 6. Flows

**Connect.** UI → `tone3000Connect`. The engine builds the authorize URL (`response_type=code`,
`client_id`, `redirect_uri=http://127.0.0.1/callback`, `code_challenge` S256, `state`, `scope=read`) and calls
`onOpenAuthWindow`. The window loads it; TONE3000 asks for email + code; the redirect arrives with `code` and
`state`. State mismatch → the callback is ignored and an error reported. Code → `/oauth/token` → tokens stored
→ `GET /user` → event `tone3000Status{connected:true, user}`. Closing the window early → `tone3000Status`
unchanged, reply `{canceled:true}`.

**Browse and add (library tab).** "Browse TONE3000" → `tone3000Browse{format:"nam"|"ir"|null}`. The engine opens
`/oauth/authorize?prompt=select_tone&preview=true&menubar=true&format=…&architecture=2` (plus the PKCE
parameters, because the callback may carry a `code` and TONE3000 recommends keeping it) in the auth window. The
callback carries `tone_id` (or `canceled=true`). Reply `{toneId}` → UI calls `tone3000Tone{id}` → the tone detail
view (§9.4) lists models (A2 list, then A1 list, both requested; "custom" architecture ignored). The user picks a
model → `tone3000Download{modelId, toneId}` (library only) or `tone3000Download{…, nodeId}` / `{…, addNode:{type,
index}}` (place it in the rig). `tone3000Download` events report progress and completion; completion with a node
target runs the existing `setAsset` / `addNode` path, so `rigLoaded` and `rigDirty` fire as they do today.

**From a node.** Node menu "Browse TONE3000…" / empty card "Browse TONE3000" → the same flow with `format` fixed
by the node type and the node as the download target. A NamAmp node accepts every NAM gear type (amp, amp-cab,
pedal, outboard, experimental); a CabIR node uses `format=ir`.

**Lists.** The tab's Favorites / Downloaded / Created / Trending / Latest call `tone3000List{list, page}` (page
size 25; trending and latest are single lists of up to 10, reported as one page). The bounded endpoints document no
format filter, so lists are not filtered server-side; each row shows its format badge, and the NAM / IR chips scope
only the picker. Results are kept only in the UI for the panel's lifetime; nothing is written to disk. Not
connected → the splash instead of lists.

**Missing asset on rig load.** `RigLoader` already marks placeholders. `Engine::rigLoadedPayload` now adds
`report.fetchable: [nodeId]` for placeholders whose `asset.source.provider == "tone3000"`. The load banner gains
"Download from TONE3000" → `tone3000Fetch{nodeIds}`; each node becomes a download job via `GET /models/{id}`
→ `model_url`. Not connected → the connect flow first, then the jobs. When the downloaded sha256 equals the rig's,
the node is assigned silently; when it differs (the creator re-uploaded), the node is assigned and the banner says
so (the next save records the new hash, like filename matches today). 404 / 403 → the node stays a placeholder and
offers "Replace from TONE3000…". A setting `tone3000AutoFetch` (default off) runs `tone3000Fetch` for every
fetchable node right after a load, for users who prefer that; it is still the user's explicit choice, so it stays
within "download only on request".

**Replace from TONE3000.** `tone3000Browse{format, nodeId, loadTone: <toneId>}` opens
`prompt=load_tone&tone_id=…`: TONE3000 shows the tone, or, if it is gone, lets the user pick a replacement (the
returned `tone_id` may differ). Then the tone detail view as above.

**Disconnect.** `tone3000Disconnect` cancels jobs, removes the tokens from the `SecureStore`, forgets the user,
and asks the app to clear the auth window's cookie store (`WebBrowserComponent::clearCookies`). Downloaded files
and their provenance stay (they are the user's library); the terms' deletion duty covers the user's data (tokens,
profile), which is gone.

## 7. Data

### 7.1 On disk

```
~/Music/AMPerator/
  Models/TONE3000/<creator>/<tone title> — <model name>.nam
  IRs/TONE3000/<creator>/<tone title> — <model name>.wav
  Art/tone3000/<toneId>.<jpg|png|webp>          # cached tone image, served via the art/ resource path
<dataDir>/tone3000/index.json                   # provenance by sha256 (§7.2)
<dataDir>/tone3000/.partial/<jobId>             # in-flight downloads, deleted on completion/start-up
Keychain item: service "com.amperator.tone3000", account <client id>  # {access, refresh, expiresAt, user id}
```

- Path components use `RigStore::sanitizeFileName`; a name collision with a different sha appends ` (2)`.
- The same model downloaded twice (same sha) is not written again; the existing file is used.
- Files are ordinary library files: `AssetIndex` indexes them like anything else, the user may move or delete them,
  and `resolve()` is unchanged (hash, then file name, then missing).
- The extension comes from the tone's `format` (`nam` → `.nam`, `ir` → `.wav`); the content is checked before the
  file is accepted (`.nam` must parse as JSON with a `version` key; `.wav`/`.aiff` must open with JUCE's audio
  format readers). A mismatch fails the job and nothing enters the library.

### 7.2 Provenance record (`index.json`, keyed by sha256)

```json
{ "9f2c…": { "provider": "tone3000", "modelId": "m_123", "toneId": "t_456",
             "title": "Mesa Dual Rec Ch2", "modelName": "Standard", "size": "standard",
             "architecture": "2", "format": "nam", "gear": "amp", "license": "cc-by",
             "creator": { "username": "jane", "displayName": "Jane", "avatarUrl": "https://…", "url": "https://www.tone3000.com/jane" },
             "toneUrl": "https://www.tone3000.com/tones/t_456", "image": "tone3000/t_456.jpg",
             "downloadedAt": "2026-10-20T18:02:11Z" } }
```

Only metadata of files the user downloaded is stored; this is attribution, not a catalog cache.

### 7.3 Rig file

Unchanged format (`version: 1`): `asset.source = {"provider": "tone3000", "id": "<model id>"}` is already in the
schema and parser. Two engine changes: `assetRefFor` (used by `setAsset` and `addNode`) looks the file's sha up in
the `ProviderIndex` and fills `source` when found, so a TONE3000 file picked from the local picker keeps its origin;
and `RigLoader::capture` keeps `source` when a node's file is matched by name.

### 7.4 Bridge shapes (additive)

- **Asset** (`listAssets`) gains `source?: {provider, id, toneId, title, creator: {username, displayName, avatarUrl, url}, license, url, image}`.
- **Rig node art** gains `providerImage`: the absolute path of the cached image in `Art/tone3000/` (served through
  the existing `art/` path; the UI's `resolveArt` passes it through `artUrl` instead of using it as a raw URL).
- **Report** gains `fetchable: [nodeId]`.
- **Settings** gains `tone3000AutoFetch: bool` (default false) and `tone3000ClientId: string` (empty = built-in).
- **Tone3000Status**: `{configured: bool, connected: bool, user: {username, displayName, avatarUrl, url} | null, busy: bool}`
  (`configured` is false when the build has no client id; `busy` while a connect or picker window is open).
- **ToneSummary**: `{id, title, gear, format, license, creator, image, url, modelsCount, downloadsCount, favoritesCount}`.
- **Tone**: ToneSummary + `{description, makes: [name], tags: [name], images: [url], publishedAt,
  models: [{id, name, size, architecture}]}` (A2 models first, then A1; `custom` omitted).
- **DownloadJob**: `{jobId, modelId, toneId, nodeId?, state: "queued"|"downloading"|"verifying"|"done"|"failed",
  progress: 0..1, file?, sha256?, error?}`.

## 8. Bridge protocol additions (version stays 1)

| Command | Args | Result | Notes / events |
|---|---|---|---|
| `tone3000Status` | — | Tone3000Status | Also part of `getState` as `tone3000`. |
| `tone3000Connect` | — | `{connected, user}` or `{canceled:true}` | Opens the auth window (`ok:false "not available"` without an app hook or client id; `"busy"` if a window is open). Replies when the window closes. Long call: UI passes a 10-minute timeout. `tone3000Status` event on change. |
| `tone3000Disconnect` | — | `{}` | Cancels jobs, wipes tokens and user, clears cookies. `tone3000Status`. |
| `tone3000Browse` | `format?` (`nam`/`ir`), `nodeId?`, `loadTone?` (tone id) | `{toneId}` or `{canceled:true}` | Opens `select_tone` (or `load_tone` when `loadTone` is given). Not connected → runs the connect flow inside the same window first. Long call (10 min). |
| `tone3000List` | `list` (`favorited`/`downloaded`/`created`/`trending`/`latest`), `page?` | `{tones: [ToneSummary], page, totalPages}` | Needs connected; `ok:false "not connected"` otherwise. `trending`/`latest` always reply `page:1, totalPages:1`. |
| `tone3000Tone` | `id` | Tone | `GET /tones/{id}` + models for architectures 2 and 1. |
| `tone3000Download` | `modelId`, `toneId?`, `nodeId?`, `addNode?: {type, index}` | `{jobId}` | Queues a job. `nodeId`/`addNode` make the finished file the node's asset (`setAsset` / `addNode` semantics, so `rigLoaded`, `rigDirty{true}`). Already in the library (same sha) → immediate `done`. Events `tone3000Download` (DownloadJob). |
| `tone3000Fetch` | `nodeIds?` (default: every fetchable node of the current rig) | `{jobs: [jobId]}` | One job per node via `GET /models/{id}`. Missing on TONE3000 → that job fails with `error: "gone"`. |
| `tone3000Cancel` | `jobId` | `{}` | Partial file deleted; `tone3000Download{state:"failed", error:"canceled"}`. |

| Event | Data | When |
|---|---|---|
| `tone3000Status` | Tone3000Status | Connected, disconnected, user loaded, token expired for good (`invalid_grant`), window opened/closed. |
| `tone3000Download` | DownloadJob | Queued, progress (≤ 4 Hz), verifying, done, failed. |

`getState` adds `tone3000: Tone3000Status`. `rigLoaded.report.fetchable` as in §7.4. Commands reply `ok:false
"not configured"` when the build has no client id.

## 9. UI

Follows TONE3000's design requirements, mapped to AMPerator's existing library panel and pedalboard.

### 9.1 Library tab "TONE3000"
The disabled "Search online — coming soon" tab becomes **TONE3000**, labelled with the full TONE3000 logo (the
first TONE3000 mark the user sees). Not configured → one line: "This build has no TONE3000 client id" (dev builds).

### 9.2 Splash (not connected)
Logo, the recommended copy ("AMPerator has partnered with TONE3000 to give you access to thousands of NAM captures
and IRs…"), "Connect TONE3000", and "Powered by TONE3000" with a link to tone3000.com (opened in the system
browser via a new native function `openExternal(url)`, allow-listed to `https://www.tone3000.com/`).

### 9.3 Connected
Header: signed-in avatar + username, "Disconnect". A primary **Browse TONE3000** button (full logo on it) and two
small filter chips (NAM / IR) that scope the picker only (the lists below are unfiltered, see §6). Sub-tabs Favorites · Downloaded · Created · Trending ·
Latest. Each row: tone image (or the archetype fallback), title, gear, format badge, creator avatar + username,
and a "T3K" origin mark on rows already in the local library. Rows open the detail view. Paged lists end with
"Discover more on TONE3000" (opens the picker). Errors (offline, rate-limited) show in place with Retry.

### 9.4 Tone detail (modal, reused for picker results and list rows)
Image, title, gear, format, creator (avatar, username, link to their TONE3000 page), license (human label, e.g.
"CC BY-NC — credit the creator, non-commercial"), description, "View on TONE3000". A model selector (size and
architecture, A2 preferred and pre-selected, "already in library" marked). Actions: **Download** (library only),
**Add to rig** (a NAM tone becomes a new NamAmp placed right after the current NamAmp, else before the first CabIR,
else at the end of the chain; an IR tone becomes a new CabIR placed right after the last NamAmp or CabIR, else at
the end), or, when opened from a node, **Use on <node>**. Progress bar while downloading; the modal may be closed and the job continues (toast on
completion).

### 9.5 Pedalboard
- Node menu: "Browse TONE3000…" for NamAmp/CabIR; an empty NamAmp/CabIR card shows "Choose model…" and "Browse
  TONE3000". A missing node with a TONE3000 source shows "Download" and "Replace from TONE3000…" beside "Locate…".
- A node whose asset has a TONE3000 source shows a small T3K mark and, in its expanded card, title and creator
  with "View on TONE3000". Art: `providerImage` (tier 2), credited in the expanded card.
- Load banner: "2 models from TONE3000 are missing. Download" when `report.fetchable` is non-empty.

### 9.6 Models / IRs tabs and settings
Rows with provenance show creator and license under the name. Settings → a "TONE3000" section: status,
Connect/Disconnect, the auto-fetch checkbox, and (dev) a client-id field.

## 10. Terms compliance checklist (what the implementation must keep true)

1. Only `select_tone`/`load_tone` prompts and the five bounded lists; `GET /tones/{id}`, `GET /models`, `GET
   /models/{id}`, `GET /user` as the documented follow-ups of those flows; model files via `model_url`.
2. Every download is one user action (click, or the opt-in auto-fetch setting) for one model.
3. No catalog data on disk other than provenance of downloaded files; list results live only in UI memory.
4. Files are never uploaded, synced or shared by the app; rigs carry only `source` references.
5. Creator, license and links are stored with each file and shown wherever the file is named (§9); nothing is
   renamed into our brand.
6. Browsing happens in TONE3000's own page; our list views carry TONE3000 branding; "Powered by TONE3000" and a
   link appear in the app, README and THIRD_PARTY.md (logo files listed there as TONE3000 trademarks used under
   its brand guidelines, not under AGPL).
7. Tokens are stored in the OS secure store, used only for the signed-in user, and deleted on disconnect.
8. Rate limits are honoured: a 429 is shown with its `Retry-After`, never retried in a loop; list calls are
   paged on demand; no polling.
9. The secret key never exists in the repo or the app; the publishable key is the only credential (§12).

## 11. Error handling

| Situation | Behaviour |
|---|---|
| No client id in the build | Tab shows the "not configured" line; commands reply `ok:false "not configured"`; nothing else changes. |
| Offline / DNS / timeout | Lists show "Can't reach TONE3000" with Retry; a download job fails with the message; rig loading and playing are unaffected. |
| User closes the auth/picker window | Reply `{canceled:true}`; no error toast. |
| `state` mismatch on callback | Callback dropped, `error{context:"tone3000"}` event "sign-in response didn't match the request"; window closed. |
| Token refresh fails (`invalid_grant`, 400/401) | Tokens removed, `tone3000Status{connected:false}`; the next action starts the connect flow. |
| 401 on a call | One refresh + retry; then as above. |
| 429 | `ok:false "TONE3000 rate limit; try again in N s"`; download job `failed` with the same text. |
| Model gone (404/403) on fetch | Job `failed "gone"`; node stays a placeholder; "Replace from TONE3000…" offered. |
| Downloaded file fails validation (not JSON / not a WAV) | Job `failed "not a valid <nam|ir> file"`; partial deleted. |
| sha differs from the rig's recorded sha | File accepted and assigned; banner "updated on TONE3000 since this rig was saved"; next save records the new sha. |
| Disk full / unwritable library | Job `failed` with the OS message; partial deleted. |
| Quit during a download | Jobs cancelled; `.partial/` cleaned at the next launch. |
| Keychain unavailable (denied) | Tokens kept in memory for the session only, with a settings note "sign-in will not be remembered". |

## 12. Build and configuration

- `AMP_TONE3000_CLIENT_ID` CMake cache variable (string) → compiled into `amp_engine` as the default client id.
  Empty by default, so a stranger's fork builds without the integration; the project's release builds set it to the
  project's publishable key (created in the maintainer's TONE3000 account under Settings → API Keys). The key is
  public by design (it is the OAuth client id); no redirect URI is registered on it, so loopback URIs stay accepted.
- `settings.json` `tone3000ClientId` overrides it at runtime (developers testing their own key).
- macOS: link `Security.framework` in `engine/` for `KeychainStore`. The app target gets `Tone3000AuthWindow`
  and the `openExternal` native function.
- Branding assets from TONE3000's logo zip go in `ui/src/brand/tone3000/` with a notice in `THIRD_PARTY.md`.

## 13. Testing

- **Unit (engine, headless, Catch2):** PKCE values and authorize URL; callback with wrong state rejected;
  code → tokens stored in `MemoryStore`; refresh 60 s early, single-flight under concurrent calls, rotation stored;
  `invalid_grant` → disconnected; 401 retried once; 429 mapped with `Retry-After`; deprecation header logged;
  list/tone/model parsing from fixture JSON; download job → file placed under `Models/TONE3000/…`, sha computed on
  the worker, `index.json` written atomically, duplicate sha reuses the file, validation failure leaves nothing;
  `listAssets` carries `source`; `setAsset` on a known file fills `source`; rig save/load round-trips `source`;
  `report.fetchable` lists the right nodes; `tone3000Fetch` assigns a node and emits `rigLoaded`; a 404 leaves the
  placeholder; disconnect cancels jobs and wipes the store; quit cleans partials.
- **Bridge:** every new command's argument validation and `not configured` / `not connected` replies, through the
  existing `bridge_test` fixture with `FakeHttpTransport`, `MemoryStore` and a fake auth-window hook.
- **UI (vitest):** tab states (not configured / splash / connected), list row attribution, detail view model
  pre-selection, node badge and banner rendering from `report.fetchable`, `resolveArt` tier-2 path handling.
- **Manual checklist additions:** first sign-in on macOS (window closes itself, Keychain prompt once), picker with
  preview players, an A2 model downloaded and playing at 48 kHz and 44.1 kHz, an IR tone, rig re-opened after
  deleting the file → one-click restore, offline launch, disconnect then reconnect without re-entering the code.

## 14. Risks and open questions

- **A2 playback:** believed supported by the vendored NAM core; the first plan task downloads a real A2 file and
  runs it through the regression harness before any UI work depends on it. Fallback: request `architecture=1`
  and show A1 models first.
- **Sub-project 4 cannot text-search TONE3000** on the free tier. The AI builder will have to pick from the user's
  lists and known model ids, or ask the user to choose in the picker, or AMPerator asks support@tone3000.com for
  access to search for a non-commercial app. Decide when designing sub-project 4; nothing here blocks either path.
- **`model_url` for IR tones** is documented only as "pre-built download URL"; the plan's first spike confirms the
  returned file is a plain WAV (the validation step in §7.1 protects the library either way).
- **Whether TONE3000's picker page works inside a JUCE `WebBrowserComponent`** (WKWebView): the official JUCE
  example ships this option, so it is expected to; if TONE3000 blocks embedded views, the fallback is the system
  browser plus a loopback `StreamingSocket` listener as in that example (same `Tone3000Auth`, different window).
- **Token lifetime** is undocumented; the client reads `expires_in` and never assumes a value.
