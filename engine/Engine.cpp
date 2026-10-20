#include "Engine.h"

#include "bridge/Bridge.h"
#include "storage/AtomicFile.h"

namespace amp
{
juce::String engineVersion()
{
    return "0.1.0";
}

namespace
{
juce::String deviceStatusName(DeviceStatus s)
{
    switch (s)
    {
        case DeviceStatus::Running: return "Running";
        case DeviceStatus::Disconnected: return "Disconnected";
        case DeviceStatus::NoDevice: break;
    }
    return "NoDevice";
}

juce::String hidStatusName(HidFootswitch::Status s)
{
    switch (s)
    {
        case HidFootswitch::Status::Connected: return "Connected";
        case HidFootswitch::Status::PermissionDenied: return "PermissionDenied";
        case HidFootswitch::Status::Busy: return "Busy";
        case HidFootswitch::Status::NotFound: break;
    }
    return "NotFound";
}
} // namespace

// Prepares the tuner for the device's rate whenever the device (re)starts, before any audio callback at the new rate.
// Registered before the AudioIO (so before the RealtimeEngine, whose analyzerTap pushes to the tuner): when it is added
// to an already running device, JUCE calls its audioDeviceAboutToStart outside the callback lock, but nothing pushes
// yet; on every later (re)start JUCE calls all aboutToStart handlers under the callback lock, which the audio callback
// also holds, so TunerTap::prepare() never overlaps TunerTap::push(). (A driver-side restart on a non-message thread
// can still overlap the message thread's poll().) As the manager's first callback it is handed the real device
// outputs; it clears them and the other callbacks' outputs are summed on top.
struct Engine::TunerPreparer : juce::AudioIODeviceCallback
{
    explicit TunerPreparer(TunerTap& t) : tuner(t) {}

    void audioDeviceIOCallbackWithContext(const float* const*, int, float* const* outputs, int numOutputs, int n,
                                          const juce::AudioIODeviceCallbackContext&) override
    {
        for (int ch = 0; ch < numOutputs; ++ch)
            if (outputs[ch] != nullptr)
                juce::FloatVectorOperations::clear(outputs[ch], n);
    }
    void audioDeviceAboutToStart(juce::AudioIODevice* d) override { tuner.prepare(d->getCurrentSampleRate()); }
    void audioDeviceStopped() override {}

    TunerTap& tuner;
};

Engine::Engine(Options o)
    : libraryRoot(o.libraryRoot == juce::File() ? RigStore::defaultLibraryRoot() : o.libraryRoot),
      dataDir(o.dataDir == juce::File() ? libraryRoot : o.dataDir),
      audioEnabled(o.startAudio),
      controlsEnabled(o.startControls),
      store(libraryRoot),
      mapping([this](const juce::String& id, std::optional<bool> set) { return applyBypass(id, set); })
{
    bridgePtr = std::make_unique<Bridge>(*this);

    for (auto* sub : { "Models", "IRs", "Rigs", "Art" })
        libraryRoot.getChildFile(sub).createDirectory();
    dataDir.createDirectory();

    settings = Settings::load(dataDir.getChildFile("settings.json"));
    userArt = juce::JSON::parse(artDir().getChildFile("user-art.json"));
    if (! userArt.isObject())
        userArt = json::object();

    applyAssetRoots();

    // Plugins are not scanned at startup (rescanPlugins does that); a leftover crash marker blacklists its plugin.
    plugins = std::make_unique<PluginHost>(dataDir, std::move(o.pluginFormats), o.addDefaultPluginFormats);
    plugins->checkCrashMarkerOnStartup();
    loader = std::make_unique<RigLoader>(assets, *plugins);

    mapping.onBypassChanged = [this](const juce::String& node, bool bypass)
    {
        bridge().emit("bypassChanged", json::object({ { "id", node }, { "bypass", bypass } }));
        setDirty(true);
    };
    mapping.onLearned = [this](const Mapping& m)
    {
        if (live)
        {
            auto& maps = live->doc.mappings;
            std::erase_if(maps, [&](const Mapping& r) { return r.control == m.control; });
            maps.push_back(m);
        }
        bridge().emit("learned", json::object({ { "control", m.control }, { "node", m.node } }));
        setDirty(true);
    };

    tuner.prepare(spec.sampleRate);
    realtime.analyzerTap = [this](const float* data, int n) { tuner.push(data, n); };

    if (audioEnabled)
    {
        if (o.deviceManager == nullptr)
            ownedManager = std::make_unique<juce::AudioDeviceManager>();
        manager = o.deviceManager != nullptr ? o.deviceManager : ownedManager.get();
        tunerPreparer = std::make_unique<TunerPreparer>(tuner);
        manager->addAudioCallback(tunerPreparer.get());
        audio = std::make_unique<AudioIO>(*manager, realtime);
        audio->onStatus = [this](DeviceStatus) { pollStatus(); };
        audio->onDeviceRestarted = [this](double sr, int bs) { rebuildForDevice(sr, bs); };
        if (! o.deferAudioStart)
            openAudioDevice(true);
    }

    loadInitialRig();

    hid.setKeyAssignments(settings.footswitchKeys);
    if (controlsEnabled)
    {
        hid.onEvent = [this](const ControlEvent& e)
        {
            if (e.control.startsWith("hidkey:"))
                handleRawKey(e.control.fromFirstOccurrenceOf(":", false, false).getIntValue());
            else
                injectControl(e);
        };
        midi.onEvent = [this](const ControlEvent& e) { injectControl(e); };
        hid.start();
        midi.start();
    }
    lastDeviceStatus = juce::JSON::toString(deviceStatusVar(), true);
    lastControlStatus = juce::JSON::toString(controlStatusVar(), true);
    if (audioEnabled || controlsEnabled)
        statusTimer.startTimer(1000);
    if (audioEnabled)
        meterTimer.startTimerHz(30);

    saveSettings();
}

void Engine::openAudioDevice(bool permissionGranted)
{
    if (audio == nullptr || audioStarted)
        return;
    audioStarted = true;
    if (! permissionGranted)
    {
        reportError(juce::String::fromUTF8("Microphone access denied \u2014 open System Settings \u2192 Privacy & "
                                           "Security \u2192 Microphone, enable AMPerator, then relaunch"),
                    "audioPermission", {}, true);
        return;
    }
    if (const auto r = audio->apply(settings); r.failed())
        reportError("Could not open the audio device: " + r.getErrorMessage(), "audioDevice", {}, true);
    audio->writeBack(settings);
}

void Engine::startAudio(bool permissionGranted)
{
    if (audio == nullptr || audioStarted)
        return;
    openAudioDevice(permissionGranted); // a successful open rebuilds the current rig via onDeviceRestarted
    saveSettings();
    pollStatus();
}

Engine::~Engine()
{
    if (live && beforeNodesReplaced)
        beforeNodesReplaced(false);
    meterTimer.stopTimer();
    tunerTimer.stopTimer();
    statusTimer.stopTimer();
    hid.stop();
    midi.stop();
    if (manager != nullptr && tunerPreparer != nullptr)
        manager->removeAudioCallback(tunerPreparer.get());
    audio.reset(); // unregisters the RealtimeEngine from the device
    if (ownedManager != nullptr)
        ownedManager->closeAudioDevice();
    realtime.analyzerTap = nullptr;
}

Bridge& Engine::bridge() { return *bridgePtr; }

void Engine::injectControl(const ControlEvent& e)
{
    bridge().emit("controlEvent", json::object({ { "control", e.control }, { "pressed", e.pressed } }));
    mapping.handle(e);
}

// ---- Rig lifecycle ----

juce::String Engine::installRig(const RigDocument& doc, bool isSaved, bool isStructuralEdit)
{
    juce::String error;
    auto built = loader->build(doc, spec, error);
    if (! built)
        return error.isNotEmpty() ? error : juce::String("rig could not be built");

    if (! realtime.swapPlan(built->plan))
    {
        const juce::String message = "the audio engine refused the new plan (sample rate mismatch)";
        if (live) // keep playing (and editing) the previous rig
            return message;
        reportError(message, "swapPlan"); // nothing to keep: install it anyway so there always is a rig
    }
    replaceLive(std::move(*built), isStructuralEdit);
    refreshMappings();
    snapshotPluginEditors(); // before rigLoaded, which carries the snapshots
    if (isStructuralEdit)
    {
        emitRigLoaded();
        setDirty(true);
    }
    else
    {
        saved = isSaved;
        mapping.cancelLearn();
        setDirty(false);
        emitRigLoaded();
    }
    return {};
}

CommandResult Engine::structuralEdit(const std::function<juce::String(RigDocument&)>& edit)
{
    if (! live)
        return CommandResult::failure("no rig loaded");
    auto doc = currentDoc();
    if (auto error = edit(doc); error.isNotEmpty())
        return CommandResult::failure(error);
    if (auto error = installRig(doc, saved, true); error.isNotEmpty())
        return CommandResult::failure(error);
    return CommandResult::success();
}

void Engine::rebuildForDevice(double sampleRate, int bufferSize)
{
    spec = { sampleRate, bufferSize };
    // Same rate and block size: the running plan survived the restart (RealtimeEngine only drops plans of another
    // rate), so don't reinstantiate every node.
    if (! live || (juce::exactlyEqual(live->plan->spec().sampleRate, sampleRate) && live->plan->spec().maxBlock == bufferSize))
        return;
    juce::String error;
    auto built = loader->build(currentDoc(), spec, error);
    if (! built)
    {
        reportError(error, "deviceRestart");
        return;
    }
    if (! realtime.swapPlan(built->plan))
    {
        reportError("the audio engine refused the rebuilt plan", "deviceRestart"); // keep the previous rig
        return;
    }
    replaceLive(std::move(*built), true);
}

void Engine::replaceLive(LiveRig&& next, bool sameRig)
{
    if (live && beforeNodesReplaced)
        beforeNodesReplaced(sameRig); // the app closes plugin editors while their instances still exist
    live = std::move(next);
}

void Engine::loadInitialRig()
{
    if (settings.lastRigId.isNotEmpty())
    {
        // On failure lastRigId is kept, so the rig stays recoverable (restoreBackup) and is retried next launch.
        auto parsed = store.load(settings.lastRigId);
        auto error = parsed.rig ? installRig(*parsed.rig, true, false) : parsed.error;
        if (error.isEmpty())
        {
            settings.noteRecent(settings.lastRigId);
            return;
        }
        reportError("Could not load the last rig: " + error, "loadRig", settings.lastRigId, true);
    }
    const auto error = installRig(makeDefaultRig("New Rig", "My Tones"), false, false);
    jassert(error.isEmpty()); // the default chain is always buildable
    juce::ignoreUnused(error);
}

void Engine::refreshMappings()
{
    if (! live)
        return;
    mapping.setMappings(live->doc.mappings, settings.globalMappings);
    juce::StringArray ids;
    for (auto& n : live->doc.nodes)
        if (n.type != NodeType::Input && n.type != NodeType::Output)
            ids.add(n.id);
    mapping.setValidNodes(ids);
}

RigDocument Engine::currentDoc() const
{
    jassert(live.has_value());
    return loader->capture(*live);
}

juce::var Engine::rigLoadedPayload() const
{
    return json::object({ { "rig", json::rig(currentDoc(), [this](const NodeDoc& n) { return artFor(n); }) },
                          { "report", json::report(live->report) },
                          { "saved", saved } });
}

void Engine::emitRigLoaded() { bridge().emit("rigLoaded", rigLoadedPayload()); }

void Engine::setDirty(bool d)
{
    if (dirty == d)
        return;
    dirty = d;
    bridge().emit("rigDirty", json::object({ { "dirty", dirty } }));
}

NodeDoc* Engine::findDocNode(const juce::String& id)
{
    if (! live)
        return nullptr;
    for (auto& n : live->doc.nodes)
        if (n.id == id)
            return &n;
    return nullptr;
}

bool Engine::isPlaceholder(const juce::String& id) const
{
    return live && live->report.placeholderNodeIds.contains(id);
}

std::optional<bool> Engine::applyBypass(const juce::String& id, std::optional<bool> set)
{
    if (isPlaceholder(id)) // runs as a bypassed passthrough until its asset/plugin is available
        return std::nullopt;
    const auto current = realtime.getBypass(id);
    if (! current)
        return std::nullopt;
    const bool next = set.value_or(! *current);
    realtime.setBypass(id, next);
    return next;
}

std::optional<AssetRef> Engine::assetRefFor(const juce::String& kind, const juce::String& file) const
{
    if (! juce::File::isAbsolutePath(file))
        return std::nullopt;
    const juce::File f(file);
    for (auto& e : assets.list(kind))
        if (e.file == f)
            return AssetRef { kind, f.getFileName(), e.sha256, std::nullopt };
    return std::nullopt;
}

void Engine::reportError(const juce::String& message, const juce::String& context, const juce::String& rigId,
                         bool atStartup)
{
    auto e = json::object({ { "message", message }, { "context", context } });
    if (rigId.isNotEmpty())
        e.getDynamicObject()->setProperty("rigId", rigId);
    if (atStartup)
        startupErrors.add(e);
    bridge().emit("error", e); // queued by the Bridge until the UI attaches
}

// ---- Settings / status ----

void Engine::saveSettings()
{
    if (! settings.save(dataDir.getChildFile("settings.json")))
        reportError("could not save settings.json", "settings");
}

void Engine::applyAssetRoots()
{
    juce::Array<juce::File> roots { libraryRoot.getChildFile("Models"), libraryRoot.getChildFile("IRs") };
    for (auto& f : settings.watchedFolders)
        if (juce::File::isAbsolutePath(f))
            roots.addIfNotAlreadyThere(juce::File(f));
    assets.setRoots(roots);
    assets.rescan(); // on the message thread (v1: watched folders are small)
}

juce::var Engine::deviceStatusVar() const
{
    return json::object({ { "status", audio ? deviceStatusName(audio->status()) : juce::String("NoDevice") },
                          { "latencyMs", audio ? audio->roundTripLatencyMs() : 0.0 },
                          { "xruns", audio ? audio->xrunCount() : -1 } });
}

juce::var Engine::controlStatusVar() const
{
    juce::StringArray midiDevices;
    if (controlsEnabled)
        for (auto& d : juce::MidiInput::getAvailableDevices())
            midiDevices.add(d.name);
    return json::object({ { "hid", hidStatusName(hid.status()) },
                          { "exclusive", hid.exclusive() },
                          { "lastKey", lastRawKey >= 0 ? juce::var(lastRawKey) : juce::var() },
                          { "assigning", pendingAssign.isEmpty() ? juce::var() : juce::var(pendingAssign) },
                          { "midiDevices", json::strings(midiDevices) } });
}

void Engine::pollStatus()
{
    const auto device = deviceStatusVar();
    if (auto text = juce::JSON::toString(device, true); text != lastDeviceStatus)
    {
        lastDeviceStatus = text;
        bridge().emit("deviceStatus", device);
    }
    const auto control = controlStatusVar();
    if (auto text = juce::JSON::toString(control, true); text != lastControlStatus)
    {
        lastControlStatus = text;
        bridge().emit("controlStatus", control);
    }
}

void Engine::pollTuner()
{
    if (const auto r = tuner.poll())
        bridge().emit("tuner", json::object({ { "active", true }, { "hz", r->hz }, { "note", r->note },
                                              { "octave", r->octave }, { "cents", r->cents } }));
    else
        bridge().emit("tuner", json::object({ { "active", false }, { "hz", 0.0 }, { "note", "" },
                                              { "octave", 0 }, { "cents", 0.0 } }));
}

void Engine::emitMeter()
{
    // The only reader of the peak getters (each read resets the peak).
    bridge().emit("meter", json::object({ { "inDb", realtime.inputPeakDb() }, { "outDb", realtime.outputPeakDb() } }));
}

// ---- User art ----

juce::File Engine::artDir() const { return libraryRoot.getChildFile("Art"); }

juce::String Engine::userImageFor(const juce::String& rigId, const juce::String& nodeId) const
{
    const auto name = userArt[juce::Identifier(rigId + "/" + nodeId)].toString();
    if (name.isEmpty())
        return {};
    const auto f = artDir().getChildFile(name);
    return f.existsAsFile() ? f.getFullPathName() : juce::String();
}

juce::var Engine::artFor(const NodeDoc& n) const
{
    const auto rigId = live ? live->doc.id : juce::String();
    juce::String snapshot;
    if (n.type == NodeType::Plugin && n.plugin && n.plugin->uid.isNotEmpty())
        if (const auto f = PluginHost::snapshotFile(artDir().getChildFile("cache"), n.plugin->uid); f.existsAsFile())
            snapshot = f.getFullPathName();
    return json::object({ { "archetype", n.artArchetype },
                          { "image", rigId.isNotEmpty() && n.id.isNotEmpty() ? userImageFor(rigId, n.id) : juce::String() },
                          { "snapshot", snapshot } });
}

void Engine::snapshotPluginEditors()
{
    // PluginHost caches one snapshot per plugin (or remembers that it rendered blank), so the editor is created
    // offscreen at most once per plugin, the first time it is loaded.
    for (const auto& n : live->doc.nodes)
    {
        if (n.type != NodeType::Plugin || ! n.plugin)
            continue;
        auto* pn = live->plan->find(n.id);
        auto* node = pn != nullptr ? dynamic_cast<PluginNode*>(pn->node.get()) : nullptr;
        if (node != nullptr && node->instance() != nullptr)
            plugins->snapshotEditor(*node->instance(), n.plugin->uid, artDir().getChildFile("cache"));
    }
}

juce::File Engine::artFileForRequest(const juce::String& path) const
{
    const juce::String prefix = "/art/";
    if (! path.startsWith(prefix))
        return {};
    const auto relative = path.substring(prefix.length());
    if (relative.isEmpty() || relative.containsAnyOf("\\:"))
        return {};

    juce::StringArray parts;
    parts.addTokens(relative, "/", "");
    auto file = artDir();
    for (const auto& part : parts)
    {
        if (part.isEmpty() || part == "." || part == ".." || part.startsWithChar('~'))
            return {};
        file = file.getChildFile(part);
        if (file.isSymbolicLink())
            return {};
    }

    static const juce::StringArray imageTypes { "png", "jpg", "jpeg", "gif", "webp", "svg" };
    if (! imageTypes.contains(file.getFileExtension().fromFirstOccurrenceOf(".", false, false).toLowerCase())
        || ! file.isAChildOf(artDir()) || ! file.existsAsFile())
        return {};
    return file;
}

CommandResult Engine::getState()
{
    return CommandResult::success(json::object({
        { "protocol", Bridge::protocolVersion },
        { "rig", json::rig(currentDoc(), [this](const NodeDoc& n) { return artFor(n); }) },
        { "report", json::report(live->report) },
        { "dirty", dirty },
        { "startupErrors", startupErrors },
        { "saved", saved },
        { "learning", mapping.isLearning() },
        { "settings", getSettings().result },
        { "deviceStatus", deviceStatusVar() },
        { "controlStatus", controlStatusVar() },
        { "tunerOn", tunerOn },
        { "favorites", json::strings(settings.favoriteRigIds) },
        { "recents", json::strings(settings.recentRigIds) },
        { "bands", json::strings(store.bands()) },
    }));
}
} // namespace amp
