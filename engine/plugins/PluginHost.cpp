#include "plugins/PluginHost.h"

namespace amp
{
namespace
{
constexpr const char* kKnownFile = "known-plugins.xml";
constexpr const char* kBlacklistFile = "blacklist.txt";
constexpr const char* kCrashMarkerFile = "crash-marker";
constexpr const char* kDeadMansPedalFile = "scan-in-progress";

// Marks a call that may take the process down: the marker is written and flushed before it, and removed
// when it returns. If it crashes instead, the marker survives for checkCrashMarkerOnStartup().
class ScopedCrashMarker
{
public:
    ScopedCrashMarker(juce::File f, const juce::String& id) : file(std::move(f))
    {
        file.deleteFile();
        juce::FileOutputStream out(file);
        if (out.openedOk())
        {
            out.writeText(id, false, false, nullptr);
            out.flush();
        }
    }
    ~ScopedCrashMarker() { file.deleteFile(); }

    ScopedCrashMarker(const ScopedCrashMarker&) = delete;
    ScopedCrashMarker& operator=(const ScopedCrashMarker&) = delete;

private:
    juce::File file;
};

juce::StringArray readIds(const juce::File& f)
{
    juce::StringArray lines;
    if (f.existsAsFile())
        f.readLines(lines);
    lines.trim();
    lines.removeEmptyStrings();
    return lines;
}

// True if every pixel of the snapshot is the same colour (or the image is empty): an editor that drew nothing.
bool isBlank(const juce::Image& image)
{
    if (! image.isValid() || image.getWidth() <= 0 || image.getHeight() <= 0)
        return true;
    const juce::Image::BitmapData px(image, juce::Image::BitmapData::readOnly);
    const auto first = px.getPixelColour(0, 0);
    for (int y = 0; y < px.height; ++y)
        for (int x = 0; x < px.width; ++x)
            if (px.getPixelColour(x, y) != first)
                return false;
    return true;
}

constexpr int kMaxSnapshotWidth = 480; // thumbnails: no point caching a full-size screenshot
constexpr int kMaxEditorSide = 8192;   // sanity limit before allocating the snapshot

// Main buses: stereo in/out if supported, else mono in/stereo out, else mono/mono; otherwise the
// plugin keeps its default layout. Plugins without an input bus only get their output negotiated.
void negotiateLayout(juce::AudioPluginInstance& plugin)
{
    using CS = juce::AudioChannelSet;
    const std::pair<CS, CS> candidates[] = { { CS::stereo(), CS::stereo() },
                                             { CS::mono(), CS::stereo() },
                                             { CS::mono(), CS::mono() } };
    for (const auto& [in, out] : candidates)
    {
        auto layout = plugin.getBusesLayout();
        if (layout.outputBuses.isEmpty())
            return;
        if (! layout.inputBuses.isEmpty())
            layout.inputBuses.getReference(0) = in;
        layout.outputBuses.getReference(0) = out;
        if (plugin.setBusesLayout(layout))
            return;
    }
}
} // namespace

//==============================================================================
PluginNode::PluginNode(std::unique_ptr<juce::AudioPluginInstance> p, juce::File marker, juce::String id)
    : plugin(std::move(p)), crashMarker(std::move(marker)), pluginId(std::move(id))
{
    negotiateLayout(*plugin);
    stereo = plugin->getChannelCountOfBus(false, 0) >= 2; // main output bus
}

PluginNode::~PluginNode()
{
    // Teardown calls into the plugin too: a crash here must blacklist it like a crash on load.
    const ScopedCrashMarker guard(crashMarker, pluginId);
    if (prepared)
        plugin->releaseResources();
    plugin.reset();
}

void PluginNode::prepare(const ProcessSpec& spec)
{
    numChannels = juce::jmax(plugin->getTotalNumInputChannels(), plugin->getTotalNumOutputChannels());
    // Channels 0/1 come from the node's io buffer; any beyond that are scratch.
    scratch.setSize(juce::jmax(0, numChannels - 2), spec.maxBlock);
    channelPtrs.assign((size_t) numChannels, nullptr);
    midi.ensureSize(2048);
    midi.clear();

    plugin->setRateAndBufferSizeDetails(spec.sampleRate, spec.maxBlock);
    if (everPrepared)
    {
        if (prepared)
            plugin->releaseResources();
        plugin->prepareToPlay(spec.sampleRate, spec.maxBlock);
    }
    else
    {
        const ScopedCrashMarker guard(crashMarker, pluginId);
        plugin->prepareToPlay(spec.sampleRate, spec.maxBlock);
        everPrepared = true;
    }
    prepared = true;
}

void PluginNode::process(juce::AudioBuffer<float>& io, int numSamples)
{
    if (! prepared || numChannels == 0 || plugin->isSuspended())
        return;

    for (int ch = 0; ch < numChannels; ++ch)
        channelPtrs[(size_t) ch] = ch < 2 ? io.getWritePointer(ch) : scratch.getWritePointer(ch - 2);
    if (scratch.getNumChannels() > 0)
        scratch.clear(0, numSamples);

    // References the channels (no copy); AudioBuffer keeps up to 31 channel pointers inline, no allocation.
    juce::AudioBuffer<float> buffer(channelPtrs.data(), numChannels, numSamples);
    midi.clear();
    plugin->processBlock(buffer, midi);
}

bool PluginNode::isStereo() const { return stereo; }

juce::MemoryBlock PluginNode::getState() const
{
    juce::MemoryBlock state;
    plugin->getStateInformation(state);
    return state;
}

void PluginNode::setState(const juce::MemoryBlock& state)
{
    if (state.getSize() > 0)
        plugin->setStateInformation(state.getData(), (int) state.getSize());
}

//==============================================================================
PluginHost::PluginHost(juce::File dir, std::vector<std::unique_ptr<juce::AudioPluginFormat>> extraFormats,
                       bool addDefaultFormats)
    : dataDir(std::move(dir))
{
    if (addDefaultFormats)
        juce::addDefaultFormatsToManager(formats);
    for (auto& f : extraFormats)
        formats.addFormat(std::move(f));

    dataDir.createDirectory();
    if (auto xml = juce::parseXML(dataDir.getChildFile(kKnownFile)))
        known.recreateFromXml(*xml);
    known.clearBlacklistedFiles(); // blacklist.txt is the source of truth

    blacklist = readIds(dataDir.getChildFile(kBlacklistFile));
    blacklist.removeDuplicates(false);
}

PluginHost::~PluginHost() = default;

void PluginHost::scan(std::function<void(float)> progress)
{
    // Absorb a pedal left by a crashed scan first, so the scanner neither reorders nor re-blacklists.
    for (const auto& id : readIds(dataDir.getChildFile(kDeadMansPedalFile)))
        addToBlacklist(id);
    dataDir.getChildFile(kDeadMansPedalFile).deleteFile();

    known.clearBlacklistedFiles();
    for (const auto& id : blacklist)
        known.addToBlacklist(id);

    const auto crashMarker = dataDir.getChildFile(kCrashMarkerFile);
    const int numFormats = formats.getNumFormats();
    for (int fi = 0; fi < numFormats; ++fi)
    {
        auto* format = formats.getFormat(fi);
        if (! format->canScanForPlugins())
            continue;

        juce::StringArray files;
        for (const auto& f : format->searchPathsForPlugins(format->getDefaultLocationsToSearch(), true, false))
            if (! blacklist.contains(f))
                files.add(f);

        juce::PluginDirectoryScanner scanner(known, *format, {}, true, dataDir.getChildFile(kDeadMansPedalFile),
                                             false);
        scanner.setFilesOrIdentifiersToScan(files);

        // The scanner walks its list from the back.
        for (int i = files.size(); --i >= 0;)
        {
            juce::String name;
            {
                const ScopedCrashMarker guard(crashMarker, files[i]);
                scanner.scanNextFile(true, name);
            }
            if (progress)
                progress(((float) fi + scanner.getProgress()) / (float) numFormats);
        }
    }

    for (const auto& id : known.getBlacklistedFiles())
        addToBlacklist(id);
    saveKnownList();
    if (progress)
        progress(1.0f);
}

void PluginHost::checkCrashMarkerOnStartup()
{
    const auto marker = dataDir.getChildFile(kCrashMarkerFile);
    if (marker.exists())
    {
        if (const auto id = marker.loadFileAsString().trim(); id.isNotEmpty())
            addToBlacklist(id);
        marker.deleteFile();
    }

    const auto pedal = dataDir.getChildFile(kDeadMansPedalFile);
    for (const auto& id : readIds(pedal))
        addToBlacklist(id);
    pedal.deleteFile();
}

juce::Array<juce::PluginDescription> PluginHost::available() const
{
    juce::Array<juce::PluginDescription> result;
    for (const auto& d : known.getTypes())
        if (! isBlacklisted(d))
            result.add(d);
    return result;
}

juce::StringArray PluginHost::blacklisted() const { return blacklist; }

void PluginHost::unblacklist(const juce::String& id)
{
    if (blacklist.contains(id))
    {
        blacklist.removeString(id);
        known.removeFromBlacklist(id);
        saveBlacklist();
    }
}

std::unique_ptr<ProcessorNode> PluginHost::create(const PluginRef& ref, const ProcessSpec& spec, juce::String& error)
{
    error.clear();
    const auto types = known.getTypes();
    const juce::PluginDescription* match = nullptr;
    for (const auto& d : types)
        if (d.createIdentifierString() == ref.uid)
        {
            match = &d;
            break;
        }
    if (match == nullptr)
        for (const auto& d : types)
            if (d.pluginFormatName == ref.format && d.name == ref.name)
            {
                match = &d;
                break;
            }

    if (match == nullptr)
    {
        error = "Plugin not found: " + (ref.name.isNotEmpty() ? ref.name : ref.uid);
        return nullptr;
    }
    if (isBlacklisted(*match))
    {
        error = "Plugin is blacklisted because it crashed AMPerator: " + match->name;
        return nullptr;
    }

    const auto id = match->createIdentifierString();
    const auto crashMarker = dataDir.getChildFile(kCrashMarkerFile);
    const ScopedCrashMarker guard(crashMarker, id);
    juce::String formatError;
    auto instance = formats.createPluginInstance(*match, spec.sampleRate, spec.maxBlock, formatError);
    if (instance == nullptr)
    {
        error = "Could not load plugin " + match->name + (formatError.isNotEmpty() ? ": " + formatError : "");
        return nullptr;
    }
    // Bus negotiation also calls into the plugin, so it stays under the marker.
    return std::make_unique<PluginNode>(std::move(instance), crashMarker, id);
}

PluginRef PluginHost::refFor(const juce::PluginDescription& d)
{
    return { d.pluginFormatName, d.createIdentifierString(), d.name, d.manufacturerName };
}

juce::File PluginHost::snapshotFile(const juce::File& cacheDir, const juce::String& uid)
{
    // Plugin ids contain spaces, slashes, colons...: keep a readable, legal name plus a hash so distinct ids
    // never collide after sanitising.
    auto name = juce::File::createLegalFileName(uid.replaceCharacters("/\\:. ", "_____")).substring(0, 80);
    return cacheDir.getChildFile(name + "-" + juce::String::toHexString(uid.hashCode64()) + ".png");
}

juce::File PluginHost::snapshotEditor(juce::AudioPluginInstance& plugin, const juce::String& uid, const juce::File& cacheDir)
{
    const auto png = snapshotFile(cacheDir, uid);
    if (png.existsAsFile())
        return png;
    const auto blankMarker = png.withFileExtension("blank");
    if (blankMarker.exists() || uid.isEmpty() || ! plugin.hasEditor() || plugin.getActiveEditor() != nullptr)
        return {};

    juce::Image image;
    {
        // Creating, painting and deleting a plugin editor all call into the plugin.
        const ScopedCrashMarker guard(dataDir.getChildFile(kCrashMarkerFile), uid);
        std::unique_ptr<juce::AudioProcessorEditor> editor(plugin.createEditorAndMakeActive());
        if (editor == nullptr)
            return {};
        const auto bounds = editor->getLocalBounds();
        if (bounds.getWidth() > 0 && bounds.getHeight() > 0 && bounds.getWidth() <= kMaxEditorSide
            && bounds.getHeight() <= kMaxEditorSide)
            image = editor->createComponentSnapshot(bounds, true, 1.0f);
        plugin.editorBeingDeleted(editor.get()); // what a host window does before deleting the editor
        editor.reset();
    }

    if (isBlank(image))
    {
        if (cacheDir.createDirectory())
            blankMarker.create();
        return {};
    }
    if (image.getWidth() > kMaxSnapshotWidth)
        image = image.rescaled(kMaxSnapshotWidth, juce::jmax(1, image.getHeight() * kMaxSnapshotWidth / image.getWidth()),
                               juce::Graphics::highResamplingQuality);

    if (! cacheDir.createDirectory())
        return {};
    const auto temp = png.withFileExtension("tmp");
    {
        temp.deleteFile();
        juce::FileOutputStream out(temp);
        juce::PNGImageFormat format;
        if (! out.openedOk() || ! format.writeImageToStream(image, out))
        {
            temp.deleteFile();
            return {};
        }
    }
    return temp.moveFileTo(png) ? png : juce::File();
}

bool PluginHost::isBlacklisted(const juce::PluginDescription& d) const
{
    return blacklist.contains(d.createIdentifierString()) || blacklist.contains(d.fileOrIdentifier);
}

void PluginHost::addToBlacklist(const juce::String& id)
{
    if (id.isEmpty() || blacklist.contains(id))
        return;
    blacklist.add(id);
    known.addToBlacklist(id);
    saveBlacklist();
}

void PluginHost::saveBlacklist() const
{
    dataDir.getChildFile(kBlacklistFile).replaceWithText(blacklist.isEmpty() ? juce::String()
                                                                             : blacklist.joinIntoString("\n") + "\n");
}

void PluginHost::saveKnownList() const
{
    if (auto xml = known.createXml())
        xml->writeTo(dataDir.getChildFile(kKnownFile));
}
} // namespace amp
