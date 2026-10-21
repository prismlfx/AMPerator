#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>

#include "dsp/Node.h"
#include "model/RigDocument.h"
#include "plugins/PluginScanner.h"

namespace amp
{
// A hosted third-party plugin as a chain node. Bus layout is negotiated at construction: stereo in/out
// if supported, else mono in/stereo out, else mono/mono (else whatever the plugin defaults to).
// Mono-input plugins read channel 0. process() does not allocate (plugins with more than 2 channels get
// preallocated scratch channels, cleared every block). isStereo() = main output bus has >= 2 channels.
// The first prepare() and teardown are crash-marker guarded; a repeat prepare() releases resources first.
class PluginNode : public ProcessorNode, private juce::AudioProcessorListener, private juce::Timer
{
public:
    PluginNode(std::unique_ptr<juce::AudioPluginInstance>, juce::File crashMarker, juce::String pluginId);
    ~PluginNode() override;

    /** Message thread. Non-null: from now on, edits made in the plugin (a parameter change, or a program / state change
        the plugin reports) call `fn` on the message thread, coalesced: the plugin's notification only sets a flag
        (any thread, lock- and allocation-free) that a message-thread timer polls every editPollMs. Earlier changes
        (e.g. setState() while the rig was built) are dropped. Null: stop listening (pending notifications are
        dropped) and stop the timer. */
    void setOnEdited(std::function<void()> fn);
    /** Message thread. Delivers a pending edit notification now (what the timer does); also used by tests. */
    void dispatchPendingEdit();

    static constexpr int editPollMs = 100;
    void prepare(const ProcessSpec&) override;
    void reset() override; // AudioProcessor::reset()
    void process(juce::AudioBuffer<float>& io, int numSamples) override;
    bool isStereo() const override;
    juce::MemoryBlock getState() const override;
    void setState(const juce::MemoryBlock&) override;

    // For editor windows (message thread only).
    juce::AudioPluginInstance* instance() const { return plugin.get(); }

private:
    // Any thread (plugins may report from their audio or UI threads): only a relaxed atomic store.
    void audioProcessorParameterChanged(juce::AudioProcessor*, int, float) override { markEdited(); }
    void audioProcessorChanged(juce::AudioProcessor*, const ChangeDetails&) override;
    void markEdited() { edited.store(true, std::memory_order_relaxed); }
    void timerCallback() override { dispatchPendingEdit(); }

    std::unique_ptr<juce::AudioPluginInstance> plugin;
    std::function<void()> onEdited;
    std::atomic<bool> edited { false };
    juce::File crashMarker;
    juce::String pluginId;
    bool everPrepared = false;
    bool prepared = false;
    bool stereo = false; // cached after bus negotiation; read on the audio thread
    int numChannels = 0; // max(plugin inputs, plugin outputs)
    juce::AudioBuffer<float> scratch; // channels beyond the node's 2
    std::vector<float*> channelPtrs;
    juce::MidiBuffer midi;
};

// Scans and instantiates VST3/AU plugins. Files in dataDir:
//   known-plugins.xml  scan results (KnownPluginList XML), loaded on construction, saved after a scan
//   blacklist.txt      one id per line: a PluginRef uid (PluginDescription::createIdentifierString) or a
//                      plugin file/identifier that crashed a scan
//   crash-marker       id of the plugin inside a risky call (instantiation, first prepareToPlay, and the scan of
//                      one file when scanning in-process); left behind only if that call took the process down
//   scan-in-progress   JUCE's PluginDirectoryScanner dead-man's pedal (in-process scans only; same purpose)
// Scanning loads every plugin once, which is where plugins crash: with setScanWorker() that happens in a worker
// process (PluginScanner.h) and the crash marker is not needed; without it (tests, no worker executable) the scan
// runs in-process under the crash marker. Instantiation (create) is always in-process, under the marker.
// All methods: message thread. scan() and create() block; startScan() does not.
class PluginHost
{
public:
    // extraFormats are added after JUCE's default formats (VST3 + AU on macOS, per build flags);
    // tests pass a fake format with addDefaultFormats = false so no system plugins are touched.
    explicit PluginHost(juce::File dataDir,
                        std::vector<std::unique_ptr<juce::AudioPluginFormat>> extraFormats = {},
                        bool addDefaultFormats = true);
    ~PluginHost(); // cancels a running startScan() (its done callback is never called) and kills its worker

    // From now on scans load plugins in worker.executable relaunched in scanner mode (OutOfProcessScanner).
    void setScanWorker(ScanWorker);

    // Scans every format's default locations, skipping blacklisted files, on the calling thread. progress gets
    // [0, 1], ending at 1. Plugins already known and unchanged are not reloaded. Returns an error message if the
    // scan could not run at all (the worker failed to launch, a scan is already running), else empty.
    juce::String scan(std::function<void(float)> progress);
    using ScanProgress = std::function<void(float, const juce::String& pluginName)>;
    using ScanDone = std::function<void(const juce::String& error)>; // error as scan() returns it
    // The same scan on a background thread; progress and done are called on the message thread (its loop must
    // run). Returns false, doing nothing, while a scan is already running.
    bool startScan(ScanProgress, ScanDone);
    bool isScanning() const;
    // Call once at launch, before scan()/create(): a leftover crash marker or dead-man's pedal entry
    // means that plugin took the app down last time; its id goes into blacklist.txt and the files are removed.
    // Returns the ids it blacklisted now (empty when the last run ended cleanly).
    juce::StringArray checkCrashMarkerOnStartup();
    // A readable name for a blacklist id: the known plugin's name, else the plugin file's name, else the id.
    juce::String displayNameFor(const juce::String& id) const;
    juce::Array<juce::PluginDescription> available() const; // excludes blacklisted
    juce::StringArray blacklisted() const;
    void unblacklist(const juce::String& id);
    // Looks up ref.uid, falling back to format + name. Null + error if missing, blacklisted or failing to
    // instantiate. The node is not prepared yet. Call on the message thread.
    std::unique_ptr<ProcessorNode> create(const PluginRef&, const ProcessSpec&, juce::String& error);
    static PluginRef refFor(const juce::PluginDescription&);

    // Node art: a PNG of the plugin's editor, cached as snapshotFile(cacheDir, uid). Returns the cached file if
    // there is one. Otherwise creates the editor offscreen, snapshots it and caches it -- unless it rendered blank
    // (most native VST3/AU views draw nothing offscreen). The attempt runs under its own marker, <name>.blank, written
    // before the editor is created and removed only after the PNG is written -- not the plugin crash marker, so a
    // crash here never blacklists the plugin. A leftover .blank (blank editor or crash) means "don't try again".
    // Does nothing while the plugin's editor is open in a window, or when it has none. Message thread only.
    juce::File snapshotEditor(juce::AudioPluginInstance&, const juce::String& uid, const juce::File& cacheDir);
    static juce::File snapshotFile(const juce::File& cacheDir, const juce::String& uid);

private:
    struct ScanJob;
    class ScanThread;
    std::shared_ptr<ScanJob> prepareScan(); // message thread: what the scan reads, copied out of this object
    void runScan(ScanJob&, const std::function<bool()>& shouldExit); // any thread; touches only the job
    void finishScan(ScanJob&); // message thread: results into known/blacklist, files saved
    void scanThreadFinished();

    bool isBlacklisted(const juce::PluginDescription&) const;
    void addToBlacklist(const juce::String& id);
    void saveBlacklist() const;
    void saveKnownList() const;

    juce::File dataDir;
    juce::AudioPluginFormatManager formats;
    juce::KnownPluginList known;
    juce::StringArray blacklist;
    ScanWorker scanWorker;
    std::shared_ptr<ScanJob> activeJob; // non-null while startScan() runs
    std::unique_ptr<ScanThread> scanThread;
};
} // namespace amp
