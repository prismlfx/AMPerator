#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <vector>

#include "assets/AssetIndex.h"
#include "audio/AudioIO.h"
#include "control/ControlEvent.h"
#include "control/HidFootswitch.h"
#include "control/MappingEngine.h"
#include "control/MidiControl.h"
#include "dsp/TunerTap.h"
#include "engine/RealtimeEngine.h"
#include "model/RigDocument.h"
#include "online/HttpTransport.h"
#include "online/SecureStore.h"
#include "plugins/PluginHost.h"
#include "storage/RigLoader.h"
#include "storage/RigStore.h"
#include "storage/Settings.h"

namespace amp
{
/** Version string of the headless engine library. */
juce::String engineVersion();
/** The TONE3000 client id compiled in (CMake cache variable AMP_TONE3000_CLIENT_ID; empty when not set). */
juce::String compiledTone3000ClientId();

class Bridge;
namespace online
{
class Tone3000Service;
}

/** Outcome of one bridge command: `result` (an object, may be void) on success, `error` otherwise. `pending`: the
    handler took a Bridge::deferReply() completion and replies through it later (see Bridge::deferReply). */
struct CommandResult
{
    bool ok = true;
    juce::var result;
    juce::String error;
    bool pending = false;

    static CommandResult success(juce::var r = {}) { return { true, std::move(r), {}, false }; }
    static CommandResult failure(juce::String e) { return { false, {}, std::move(e), false }; }
    static CommandResult deferred() { return { true, {}, {}, true }; }
};

/** Fields of setSettings; absent fields are left unchanged. */
struct SettingsPatch
{
    std::optional<juce::String> audioDeviceType, audioDeviceName;
    std::optional<int> inputChannel, outputFirstChannel, bufferSize;
    std::optional<double> sampleRate;
    std::optional<juce::StringArray> watchedFolders;
    std::optional<std::vector<Mapping>> globalMappings;
    std::optional<std::map<int, juce::String>> footswitchKeys;
    std::optional<std::optional<UsbIds>> footswitchDevice; // set to nullopt-inside = clear
    std::optional<bool> tone3000AutoFetch;
    std::optional<juce::String> tone3000ClientId;
};

/** The facade the app owns: wires every engine unit together and serves the JSON bridge.

    Threading: everything (construction, commands, events, timers) runs on the message thread. Rig builds
    (RigLoader::build) run synchronously on the message thread (PluginHost needs it; AssetIndex/RigStore are
    single-threaded), so loading a rig briefly blocks the UI. AssetIndex rescans (watched folders) also run on
    the message thread. rescanPlugins runs the plugin scan on a background thread (loading the plugins in a worker
    process when Options::pluginScanWorker is set) and reports through events.

    Plans are built at the running device's spec; when the device (re)starts at a new rate/buffer the current
    rig is rebuilt and swapped. Without audio (startAudio = false) plans use ProcessSpec { 48000, 512 }. */
class Engine
{
public:
    struct Options
    {
        juce::File libraryRoot;   // Models/, IRs/, Rigs/<Band>/, Art/; empty = RigStore::defaultLibraryRoot()
        juce::File dataDir;       // settings.json and the plugin host files; empty = libraryRoot
        bool startAudio = true, startControls = true;
        std::vector<std::unique_ptr<juce::AudioPluginFormat>> pluginFormats; // added to PluginHost
        bool addDefaultPluginFormats = true;
        /** Plugin scans load every plugin in this executable, relaunched in scanner mode (plugins/PluginScanner.h),
            so a plugin that crashes while scanned never takes the app down. File() = in-process. The app passes
            itself; a plugin still loading after pluginScanTimeoutMs is killed and disabled. */
        juce::File pluginScanWorker;
        int pluginScanTimeoutMs = 60000;
        juce::AudioDeviceManager* deviceManager = nullptr; // not owned; null = the Engine owns one (startAudio only)
        /** With startAudio: set up audio but leave the device closed until startAudio() is called (the app asks for
            microphone permission first, so the message thread never blocks on the OS permission prompt). */
        bool deferAudioStart = false;
        /** How deleteRig disposes of rig files; null = the system trash (juce::File::moveToTrash). */
        std::function<bool(const juce::File&)> trashFile;
        /** TONE3000 network access; null = no network yet (every request fails "network transport not available"). */
        std::unique_ptr<online::HttpTransport> httpTransport;
        /** Where TONE3000 tokens live; null = the platform store (Keychain service "com.amperator.tone3000"). */
        std::unique_ptr<online::SecureStore> secureStore;
        /** TONE3000 client id; empty = settings.tone3000ClientId, else compiledTone3000ClientId(). Read once here: a
            later setSettings{tone3000ClientId} takes effect at the next launch. All empty = not configured. */
        juce::String tone3000ClientId;
    };

    explicit Engine(Options);
    ~Engine();

    Bridge& bridge();

    /** Opens the audio device from the settings (Options::deferAudioStart). Only the first call does anything.
        permissionGranted = false (microphone access denied) leaves audio closed and records the startup error
        `audioPermission` with the steps to fix it. */
    void startAudio(bool permissionGranted = true);

    /** The Engine cannot own windows: the app sets this to open a hosted plugin's editor. */
    std::function<void(juce::AudioPluginInstance*, const juce::String& nodeId)> onOpenPluginEditor;
    /** Called just before the live rig's nodes are replaced -- a rig load/new rig (sameRig = false), a structural
        edit or a device-rate rebuild of the current rig (sameRig = true) -- and from ~Engine. Every plugin instance
        handed to onOpenPluginEditor is destroyed after this returns, so the app must close their editors here. */
    std::function<void(bool sameRig)> beforeNodesReplaced;
    /** The app opens the TONE3000 sign-in window at `url` and calls done(query) with the query of the first request
        starting with `redirectPrefix`, or done("") when the user closes the window. Null = "not available". */
    std::function<void(const juce::String& url, const juce::String& redirectPrefix,
                       std::function<void(const juce::String& query)> done)>
        onOpenAuthWindow;
    /** The app clears the sign-in window's cookie store (tone3000Disconnect). */
    std::function<void()> onClearAuthCookies;

    /** The file the app's resource provider serves for a web view request `path` ("/art/<relative path>"): an
        existing image (png, jpg, jpeg, gif, webp, svg) inside the library's Art/ folder. Anything else -- other
        prefixes, "..", empty or absolute components, backslashes, symlinks, other file types -- gives File().
        Thread-safe (reads only the library root). */
    juce::File artFileForRequest(const juce::String& path) const;

    /** As if a footswitch/MIDI control produced `e` (test hook; also the path real controls take). */
    void injectControl(const ControlEvent& e);
    /** As if the footswitch reported keycode `code` going down (test hook; real presses arrive as "hidkey:<code>"). */
    void injectRawKey(int code);
    /** Pretend the footswitch is in status `s` from now on (test hook for connect/disconnect). */
    void injectHidStatus(HidFootswitch::Status s);
    /** Spec the current plan was built with (test support). */
    ProcessSpec planSpec() const { return spec; }
    /** Test support: delivers pending "edited in the plugin" notifications now instead of on the message loop. */
    void dispatchPendingPluginChanges();

    // ---- Quit (message thread) ----
    bool isDirty() const { return dirty; }
    /** The app's quit request (Cmd-Q, window close). True: nothing unsaved, quit now. False: the rig has unsaved
        changes, so `quitRequested` was emitted and the UI answers with confirmQuit (the app falls back to a native
        dialog that calls confirmQuit itself if the UI does not answer). */
    bool requestQuit();
    bool quitPending() const { return quitIsPending; }
    /** Called (message thread) when confirmQuit decided to quit: after a successful save, or on discard. */
    std::function<void()> onQuitConfirmed;

    // ---- Commands (message thread), dispatched by Bridge ----
    CommandResult getState();
    CommandResult listRigs();
    CommandResult loadRig(const juce::String& id);
    CommandResult newRig(const juce::String& name, const juce::String& band);
    /** Non-blank name/band (trimmed) rename the rig / move it to that band before it is written. asNew: save a copy
        under a fresh id (the original file is left untouched) and make it the current rig (rigLoaded). */
    CommandResult saveRig(const juce::String& name = {}, const juce::String& band = {}, bool asNew = false);
    CommandResult deleteRig(const juce::String& id);
    CommandResult stepRig(int direction); // +1 nextRig, -1 prevRig
    CommandResult addNode(NodeType, int index, const juce::var& plugin, const juce::String& assetFile);
    CommandResult moveNode(const juce::String& id, int index);
    CommandResult removeNode(const juce::String& id);
    CommandResult setParam(const juce::String& id, const juce::String& name, float value);
    CommandResult toggleBypass(const juce::String& id);
    CommandResult setAsset(const juce::String& id, const juce::String& file);
    CommandResult learnMapping(const juce::String& id);
    CommandResult cancelLearn();
    /** Removes the current rig's mapping of `control` (global mappings stay) -> mappingCleared, rigDirty. */
    CommandResult clearMapping(const juce::String& control);
    /** control = footswitch:L|M|R. The next HID key pressed becomes that control's key (replacing its previous key);
        the press itself does not trigger anything. Persists settings and emits controlStatus. */
    CommandResult assignFootswitchKey(const juce::String& control);
    CommandResult cancelAssignFootswitchKey();
    CommandResult listAssets(const juce::String& kind);
    CommandResult listPlugins();
    /** Starts a background scan (one at a time): `pluginScanProgress` events, then `pluginsChanged` with the new
        list. Result: {scanning: true} right away. */
    CommandResult rescanPlugins();
    CommandResult listBlacklisted();
    /** Removes `id` from the blacklist (the plugin may need a rescan to show up again). Result: listBlacklisted. */
    CommandResult unblacklistPlugin(const juce::String& id);
    /** Connected keyboard-type HID devices, for picking the footswitch manually (empty while controls are off). */
    CommandResult listHidKeyboards();
    CommandResult openPluginEditor(const juce::String& id);
    CommandResult getSettings();
    CommandResult setSettings(const SettingsPatch&);
    CommandResult setTuner(bool on);
    CommandResult toggleFavorite(const juce::String& id);
    /** Replaces the current rig's tags (trimmed, non-empty, <= maxTagLength characters, no control characters;
        duplicates dropped case-insensitively keeping the first spelling; at most maxTags) -> tagsChanged, rigDirty{true}.
        An unchanged list changes nothing. Invalid tags -> failure, rig untouched. */
    CommandResult setTags(const juce::StringArray& tags);
    /** Every tag of the saved rigs, merged case-insensitively (first spelling in list order), counted per rig and
        sorted by count (descending), then name. */
    CommandResult listTags();
    static constexpr int maxTags = 32, maxTagLength = 40;
    CommandResult restoreBackup(const juce::String& id);
    CommandResult setNodeArt(const juce::String& id, const std::optional<juce::String>& archetype,
                             const std::optional<juce::String>& imageFile);
    /** action = "save" (save, then quit), "discard" (quit) or "cancel" (stay). */
    CommandResult confirmQuit(const juce::String& action);

    // TONE3000 (CommandsOnline.cpp). Without a client id every one but tone3000Status fails "not configured".
    CommandResult tone3000Status(); // Tone3000Status
    /** Deferred: replies when the sign-in window is done, {connected, user} or {canceled: true}. */
    CommandResult tone3000Connect();
    CommandResult tone3000Disconnect(); // {}

private:
    struct TunerPreparer;

    // Rig lifecycle
    juce::String installRig(const RigDocument&, bool saved, bool structuralEdit);
    void replaceLive(LiveRig&&, bool sameRig);
    void watchPluginEdits(); // the live rig's plugins mark the rig dirty when edited in their own UI
    CommandResult structuralEdit(const std::function<juce::String(RigDocument&)>& edit);
    void rebuildForDevice(double sampleRate, int bufferSize);
    void loadInitialRig();
    void openAudioDevice(bool permissionGranted);
    void refreshMappings();
    RigDocument currentDoc() const;
    juce::var rigLoadedPayload() const;
    void emitRigLoaded();
    void setDirty(bool);
    NodeDoc* findDocNode(const juce::String& id);
    bool isPlaceholder(const juce::String& id) const;
    std::optional<bool> applyBypass(const juce::String& id, std::optional<bool> set);
    std::optional<AssetRef> assetRefFor(const juce::String& kind, const juce::String& file) const;
    void reportError(const juce::String& message, const juce::String& context, const juce::String& rigId = {},
                     bool atStartup = false);

    // Settings / status
    void saveSettings();
    void applyAssetRoots();
    juce::var deviceStatusVar() const;
    juce::var pluginsVar(); // listPlugins' array
    juce::var controlStatusVar() const;
    void pollStatus();
    void handleRawKey(int code);
    HidFootswitch::Status currentHidStatus() const { return hidStatusOverride ? *hidStatusOverride : hid.status(); }
    void pollTuner();
    void emitMeter();

    // Node art: user images (Art/user-art.json, keyed "<rigId>/<nodeId>") and plugin editor snapshots (Art/cache/)
    juce::File artDir() const;
    juce::String userImageFor(const juce::String& rigId, const juce::String& nodeId) const;
    juce::var artFor(const NodeDoc&) const;
    void snapshotPluginEditors();

    const juce::File libraryRoot, dataDir;
    const bool audioEnabled, controlsEnabled;

    Settings settings;
    AssetIndex assets;
    std::unique_ptr<PluginHost> plugins;
    RigStore store;
    std::unique_ptr<RigLoader> loader;
    TunerTap tuner;
    RealtimeEngine realtime;

    std::unique_ptr<juce::AudioDeviceManager> ownedManager;
    juce::AudioDeviceManager* manager = nullptr;
    std::unique_ptr<AudioIO> audio;
    std::unique_ptr<TunerPreparer> tunerPreparer;

    HidFootswitch hid;
    std::optional<HidFootswitch::Status> hidStatusOverride;
    HidFootswitch::Status lastHidStatus = HidFootswitch::Status::NotFound;
    juce::uint32 assignStartedMs = 0;
    int lastRawKey = -1;        // last HID keycode seen going down (assigned or not), -1 = none yet
    juce::String pendingAssign; // footswitch control waiting for its key, or empty
    MidiControl midi;
    MappingEngine mapping;

    ProcessSpec spec { 48000.0, 512 };
    std::optional<LiveRig> live;
    bool dirty = false, saved = false, tunerOn = false, audioStarted = false, quitIsPending = false;
    juce::var userArt; // object: "<rigId>/<nodeId>" -> file name in Art/
    juce::Array<juce::var> startupErrors; // {message, context, rigId?}: problems found while starting (getState)

    juce::String lastDeviceStatus, lastControlStatus; // JSON, to emit only on change
    juce::TimedCallback meterTimer { [this] { emitMeter(); } };
    juce::TimedCallback tunerTimer { [this] { pollTuner(); } };
    juce::TimedCallback statusTimer { [this] { pollStatus(); } };

    // TONE3000: the service references the transport, the store and `assets`, so it is declared after them (and
    // destroyed first in ~Engine). Not configured: no service, and only injected units (unused).
    std::unique_ptr<online::HttpTransport> http;
    std::unique_ptr<online::SecureStore> secureStore;
    std::unique_ptr<online::Tone3000Service> tone3000;

    std::unique_ptr<Bridge> bridgePtr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Engine)
};
} // namespace amp
