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
    setOnEdited(nullptr);
    // Teardown calls into the plugin too: a crash here must blacklist it like a crash on load.
    const ScopedCrashMarker guard(crashMarker, pluginId);
    if (prepared)
        plugin->releaseResources();
    plugin.reset();
}

void PluginNode::setOnEdited(std::function<void()> fn)
{
    const bool wasListening = onEdited != nullptr;
    onEdited = std::move(fn);
    if (onEdited != nullptr && ! wasListening)
        plugin->addListener(this);
    else if (onEdited == nullptr && wasListening)
        plugin->removeListener(this);
    edited.store(false, std::memory_order_relaxed);
    if (onEdited != nullptr)
        startTimer(editPollMs);
    else
        stopTimer();
}

void PluginNode::dispatchPendingEdit()
{
    if (edited.exchange(false, std::memory_order_relaxed) && onEdited)
        onEdited();
}

void PluginNode::audioProcessorChanged(juce::AudioProcessor*, const ChangeDetails& details)
{
    // Latency or parameter-info updates are not edits; a program or non-parameter state change is.
    if (details.programChanged || details.nonParameterStateChanged)
        markEdited();
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

void PluginNode::reset()
{
    if (prepared)
        plugin->reset();
}

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

// Everything one scan needs, so the scanning thread never touches the host: the per-format file lists, the
// list it adds to (seeded from `known`, so unchanged plugins are skipped), and the out-of-process scanner.
struct PluginHost::ScanJob
{
    struct Work
    {
        juce::AudioPluginFormat* format;
        juce::StringArray files;
    };
    std::vector<Work> work;
    juce::KnownPluginList found;
    OutOfProcessScanner* outOfProcess = nullptr; // owned by `found`; null = in-process
    juce::File crashMarker, pedal;               // in-process only
    ScanProgress progress;                       // called from runScan on its thread
    ScanDone done;
    juce::String error;
};

class PluginHost::ScanThread final : public juce::Thread
{
public:
    ScanThread(PluginHost& h, std::shared_ptr<ScanJob> j) : Thread("Plugin scan"), host(h), job(std::move(j)) {}
    ~ScanThread() override { stopThread(-1); }

    void run() override
    {
        host.runScan(*job, [this] { return threadShouldExit(); });
        // The host may be gone by the time this runs: the job expires with it.
        juce::MessageManager::callAsync([h = &host, weak = std::weak_ptr<ScanJob>(job)]
        {
            if (auto j = weak.lock())
                h->scanThreadFinished();
        });
    }

private:
    PluginHost& host;
    std::shared_ptr<ScanJob> job;
};

PluginHost::~PluginHost()
{
    scanThread.reset(); // joins; the scanner's worker is killed with the job
    activeJob.reset();
}

void PluginHost::setScanWorker(ScanWorker w) { scanWorker = std::move(w); }

bool PluginHost::isScanning() const { return activeJob != nullptr; }

std::shared_ptr<PluginHost::ScanJob> PluginHost::prepareScan()
{
    auto job = std::make_shared<ScanJob>();
    const bool inProcess = scanWorker.executable == juce::File();
    job->crashMarker = dataDir.getChildFile(kCrashMarkerFile);
    if (inProcess)
        job->pedal = dataDir.getChildFile(kDeadMansPedalFile);

    // Absorb a pedal left by a crashed in-process scan first, so the scanner neither reorders nor re-blacklists.
    for (const auto& id : readIds(dataDir.getChildFile(kDeadMansPedalFile)))
        addToBlacklist(id);
    dataDir.getChildFile(kDeadMansPedalFile).deleteFile();

    // Seeded with what is known (unchanged plugins are then skipped) but without the blacklist: blacklisted files
    // are left out of the file lists below, and the only ids the scan adds to its own blacklist are the ones it
    // found crashing -- so a Retry (unblacklist) made while the scan runs survives finishScan.
    if (auto xml = known.createXml())
        job->found.recreateFromXml(*xml);
    job->found.clearBlacklistedFiles();
    if (! inProcess)
    {
        auto scanner = std::make_unique<OutOfProcessScanner>(scanWorker);
        job->outOfProcess = scanner.get();
        job->found.setCustomScanner(std::move(scanner));
    }

    for (int fi = 0; fi < formats.getNumFormats(); ++fi)
    {
        auto* format = formats.getFormat(fi);
        if (! format->canScanForPlugins())
            continue;
        ScanJob::Work w { format, {} };
        for (const auto& f : format->searchPathsForPlugins(format->getDefaultLocationsToSearch(), true, false))
            if (! blacklist.contains(f))
                w.files.add(f);
        job->work.push_back(std::move(w));
    }
    return job;
}

// The formats are shared with the message thread (create() may run meanwhile): the scanner only calls their
// stateless queries (searchPathsForPlugins, getNameOfPluginFromIdentifier, pluginNeedsRescanning,
// doesPluginStillExist), and ~PluginHost joins this thread before `formats` goes away.
void PluginHost::runScan(ScanJob& job, const std::function<bool()>& shouldExit)
{
    const auto numFormats = (float) juce::jmax<size_t>(1, job.work.size());
    if (job.outOfProcess != nullptr)
        job.outOfProcess->setShouldExit(shouldExit);
    for (size_t fi = 0; fi < job.work.size(); ++fi)
    {
        auto& w = job.work[fi];
        juce::PluginDirectoryScanner scanner(job.found, *w.format, {}, true, job.pedal, false);
        scanner.setFilesOrIdentifiersToScan(w.files);

        // The scanner walks its list from the back.
        for (int i = w.files.size(); --i >= 0;)
        {
            if (shouldExit())
                return;
            juce::String name;
            if (job.outOfProcess != nullptr)
                scanner.scanNextFile(true, name);
            else
            {
                const ScopedCrashMarker guard(job.crashMarker, w.files[i]);
                scanner.scanNextFile(true, name);
            }
            if (job.progress)
            {
                // VST3 reports the bundle path as the name; AU reports the plugin name.
                if (juce::File::isAbsolutePath(name))
                    name = juce::File(name).getFileNameWithoutExtension();
                job.progress(((float) fi + scanner.getProgress()) / numFormats, name);
            }
        }
    }
    if (job.outOfProcess != nullptr)
        job.outOfProcess->stopWorker();
    if (shouldExit())
        return;

    // Plugins that were uninstalled since the last scan.
    for (const auto& d : job.found.getTypes())
        for (const auto& w : job.work)
            if (w.format->getName() == d.pluginFormatName && ! w.format->doesPluginStillExist(d))
                job.found.removeType(d);

    if (job.outOfProcess != nullptr)
        job.error = job.outOfProcess->launchError();
    if (job.progress)
        job.progress(1.0f, {});
}

void PluginHost::finishScan(ScanJob& job)
{
    known.clear();
    for (const auto& d : job.found.getTypes())
        known.addType(d);
    for (const auto& id : job.found.getBlacklistedFiles()) // only what this scan found crashing
        addToBlacklist(id);
    known.clearBlacklistedFiles();
    for (const auto& id : blacklist)
        known.addToBlacklist(id);
    saveKnownList();
}

juce::String PluginHost::scan(std::function<void(float)> progress)
{
    if (isScanning())
        return "a plugin scan is already running";
    auto job = prepareScan();
    if (progress)
        job->progress = [&progress](float p, const juce::String&) { progress(p); };
    runScan(*job, [] { return false; });
    finishScan(*job);
    return job->error;
}

bool PluginHost::startScan(ScanProgress progress, ScanDone done)
{
    if (isScanning())
        return false;
    activeJob = prepareScan();
    activeJob->done = std::move(done);
    if (progress)
    {
        // From the scan thread to the message thread; dropped once the job is over (host destroyed).
        activeJob->progress = [report = std::move(progress), weak = std::weak_ptr<ScanJob>(activeJob)]
            (float p, const juce::String& name)
        {
            juce::MessageManager::callAsync([report, weak, p, name]
            {
                if (auto j = weak.lock())
                    report(p, name);
            });
        };
    }
    scanThread = std::make_unique<ScanThread>(*this, activeJob);
    scanThread->startThread();
    return true;
}

void PluginHost::scanThreadFinished()
{
    auto job = std::move(activeJob);
    scanThread.reset(); // run() has returned; joins
    if (job == nullptr)
        return;
    finishScan(*job);
    if (job->done)
        job->done(job->error);
}

juce::StringArray PluginHost::checkCrashMarkerOnStartup()
{
    juce::StringArray added;
    const auto note = [&](const juce::String& id)
    {
        if (id.isNotEmpty() && ! blacklist.contains(id))
            added.add(id);
        addToBlacklist(id);
    };
    const auto marker = dataDir.getChildFile(kCrashMarkerFile);
    if (marker.exists())
    {
        note(marker.loadFileAsString().trim());
        marker.deleteFile();
    }

    const auto pedal = dataDir.getChildFile(kDeadMansPedalFile);
    for (const auto& id : readIds(pedal))
        note(id);
    pedal.deleteFile();
    return added;
}

juce::String PluginHost::displayNameFor(const juce::String& id) const
{
    for (const auto& d : known.getTypes())
        if (d.createIdentifierString() == id || d.fileOrIdentifier == id)
            return d.name;
    if (juce::File::isAbsolutePath(id))
        return juce::File(id).getFileNameWithoutExtension();
    return id;
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

    // The snapshot is optional: a plugin whose editor crashes offscreen must not be blacklisted for audio. So it runs
    // under its own marker instead of the plugin crash marker: <name>.blank is written first and removed only after a
    // PNG was written. A leftover .blank (blank editor, or a crash in here) means "never snapshot this plugin again".
    if (! cacheDir.createDirectory() || ! blankMarker.replaceWithText(uid))
        return {};
    juce::Image image;
    {
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
        return {};
    if (image.getWidth() > kMaxSnapshotWidth)
        image = image.rescaled(kMaxSnapshotWidth, juce::jmax(1, image.getHeight() * kMaxSnapshotWidth / image.getWidth()),
                               juce::Graphics::highResamplingQuality);

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
    if (! temp.moveFileTo(png))
        return {};
    blankMarker.deleteFile();
    return png;
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
