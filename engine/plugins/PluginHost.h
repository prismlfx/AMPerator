#pragma once

#include <functional>
#include <memory>
#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>

#include "dsp/Node.h"
#include "model/RigDocument.h"

namespace amp
{
// A hosted third-party plugin as a chain node. Bus layout is negotiated at construction: stereo in/out
// if supported, else mono in/stereo out, else mono/mono (else whatever the plugin defaults to).
// Mono-input plugins read channel 0. process() does not allocate (plugins with more than 2 channels get
// preallocated scratch channels, cleared every block). isStereo() = main output bus has >= 2 channels.
// The first prepare() and teardown are crash-marker guarded; a repeat prepare() releases resources first.
class PluginNode : public ProcessorNode
{
public:
    PluginNode(std::unique_ptr<juce::AudioPluginInstance>, juce::File crashMarker, juce::String pluginId);
    ~PluginNode() override;

    void prepare(const ProcessSpec&) override;
    void process(juce::AudioBuffer<float>& io, int numSamples) override;
    bool isStereo() const override;
    juce::MemoryBlock getState() const override;
    void setState(const juce::MemoryBlock&) override;

    // For editor windows (message thread only).
    juce::AudioPluginInstance* instance() const { return plugin.get(); }

private:
    std::unique_ptr<juce::AudioPluginInstance> plugin;
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

// Scans and instantiates VST3/AU plugins (in-process). Files in dataDir:
//   known-plugins.xml  scan results (KnownPluginList XML), loaded on construction, saved after scan()
//   blacklist.txt      one id per line: a PluginRef uid (PluginDescription::createIdentifierString) or a
//                      plugin file/identifier that crashed a scan
//   crash-marker       id of the plugin inside a risky call (scan of one file, instantiation, first
//                      prepareToPlay); left behind only if that call took the process down
//   scan-in-progress   JUCE's PluginDirectoryScanner dead-man's pedal (same purpose, JUCE's mechanism)
// All methods: message thread. scan() and create() block.
class PluginHost
{
public:
    // extraFormats are added after JUCE's default formats (VST3 + AU on macOS, per build flags);
    // tests pass a fake format with addDefaultFormats = false so no system plugins are touched.
    explicit PluginHost(juce::File dataDir,
                        std::vector<std::unique_ptr<juce::AudioPluginFormat>> extraFormats = {},
                        bool addDefaultFormats = true);
    ~PluginHost();

    // Scans every format's default locations, skipping blacklisted files. progress gets [0, 1], ending at 1.
    void scan(std::function<void(float)> progress);
    // Call once at launch, before scan()/create(): a leftover crash marker or dead-man's pedal entry
    // means that plugin took the app down last time; its id goes into blacklist.txt and the files are removed.
    void checkCrashMarkerOnStartup();
    juce::Array<juce::PluginDescription> available() const; // excludes blacklisted
    juce::StringArray blacklisted() const;
    void unblacklist(const juce::String& id);
    // Looks up ref.uid, falling back to format + name. Null + error if missing, blacklisted or failing to
    // instantiate. The node is not prepared yet. Call on the message thread.
    std::unique_ptr<ProcessorNode> create(const PluginRef&, const ProcessSpec&, juce::String& error);
    static PluginRef refFor(const juce::PluginDescription&);

    // Node art: a PNG of the plugin's editor, cached as snapshotFile(cacheDir, uid). Returns the cached file if
    // there is one. Otherwise creates the editor offscreen (under the crash marker), snapshots it and caches it --
    // unless it rendered blank (most native VST3/AU views draw nothing offscreen): that is remembered next to it
    // (<name>.blank) and nothing is returned, so the caller falls back to built-in art. Does nothing while the
    // plugin's editor is open in a window, or when it has none. Message thread only.
    juce::File snapshotEditor(juce::AudioPluginInstance&, const juce::String& uid, const juce::File& cacheDir);
    static juce::File snapshotFile(const juce::File& cacheDir, const juce::String& uid);

private:
    bool isBlacklisted(const juce::PluginDescription&) const;
    void addToBlacklist(const juce::String& id);
    void saveBlacklist() const;
    void saveKnownList() const;

    juce::File dataDir;
    juce::AudioPluginFormatManager formats;
    juce::KnownPluginList known;
    juce::StringArray blacklist;
};
} // namespace amp
