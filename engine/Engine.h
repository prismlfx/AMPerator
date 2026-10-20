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
#include "plugins/PluginHost.h"
#include "storage/RigLoader.h"
#include "storage/RigStore.h"
#include "storage/Settings.h"

namespace amp
{
/** Version string of the headless engine library. */
juce::String engineVersion();

class Bridge;

/** Outcome of one bridge command: `result` (an object, may be void) on success, `error` otherwise. */
struct CommandResult
{
    bool ok = true;
    juce::var result;
    juce::String error;

    static CommandResult success(juce::var r = {}) { return { true, std::move(r), {} }; }
    static CommandResult failure(juce::String e) { return { false, {}, std::move(e) }; }
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
};

/** The facade the app owns: wires every engine unit together and serves the JSON bridge.

    Threading: everything (construction, commands, events, timers) runs on the message thread. Rig builds
    (RigLoader::build) run synchronously on the message thread (PluginHost needs it; AssetIndex/RigStore are
    single-threaded), so loading a rig briefly blocks the UI. AssetIndex rescans (watched folders) also run on
    the message thread. rescanPlugins blocks the message thread for the whole plugin scan.

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
        juce::AudioDeviceManager* deviceManager = nullptr; // not owned; null = the Engine owns one (startAudio only)
        /** With startAudio: set up audio but leave the device closed until startAudio() is called (the app asks for
            microphone permission first, so the message thread never blocks on the OS permission prompt). */
        bool deferAudioStart = false;
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

    // ---- Commands (message thread), dispatched by Bridge ----
    CommandResult getState();
    CommandResult listRigs();
    CommandResult loadRig(const juce::String& id);
    CommandResult newRig(const juce::String& name, const juce::String& band);
    /** Non-blank name/band (trimmed) rename the rig / move it to that band before it is written. */
    CommandResult saveRig(const juce::String& name = {}, const juce::String& band = {});
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
    /** control = footswitch:L|M|R. The next HID key pressed becomes that control's key (replacing its previous key);
        the press itself does not trigger anything. Persists settings and emits controlStatus. */
    CommandResult assignFootswitchKey(const juce::String& control);
    CommandResult cancelAssignFootswitchKey();
    CommandResult listAssets(const juce::String& kind);
    CommandResult listPlugins();
    CommandResult rescanPlugins();
    CommandResult openPluginEditor(const juce::String& id);
    CommandResult getSettings();
    CommandResult setSettings(const SettingsPatch&);
    CommandResult setTuner(bool on);
    CommandResult toggleFavorite(const juce::String& id);
    CommandResult restoreBackup(const juce::String& id);
    CommandResult setNodeArt(const juce::String& id, const std::optional<juce::String>& archetype,
                             const std::optional<juce::String>& imageFile);

private:
    struct TunerPreparer;

    // Rig lifecycle
    juce::String installRig(const RigDocument&, bool saved, bool structuralEdit);
    void replaceLive(LiveRig&&, bool sameRig);
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
    juce::var controlStatusVar() const;
    void pollStatus();
    void handleRawKey(int code);
    void cancelPendingAssign();
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
    bool dirty = false, saved = false, tunerOn = false, audioStarted = false;
    juce::var userArt; // object: "<rigId>/<nodeId>" -> file name in Art/
    juce::Array<juce::var> startupErrors; // {message, context, rigId?}: problems found while starting (getState)

    juce::String lastDeviceStatus, lastControlStatus; // JSON, to emit only on change
    juce::TimedCallback meterTimer { [this] { emitMeter(); } };
    juce::TimedCallback tunerTimer { [this] { pollTuner(); } };
    juce::TimedCallback statusTimer { [this] { pollStatus(); } };

    std::unique_ptr<Bridge> bridgePtr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Engine)
};
} // namespace amp
