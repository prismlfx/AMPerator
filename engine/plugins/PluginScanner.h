#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>

namespace amp
{
/** Command-line id of the plugin-scan worker mode. The scanner relaunches an executable as
    `<exe> --amperator-plugin-scanner:<pipe>`; that process must serve a PluginScannerWorker instead of starting
    normally (the app's initialise() and the test binary's main() both check PluginScannerWorker::isWorkerCommandLine). */
constexpr const char* kPluginScannerUid = "amperator-plugin-scanner";

struct ScanWorker
{
    juce::File executable; // relaunched in worker mode; File() = scan in-process
    int timeoutMs = 60000; // a plugin still loading after this is killed and blacklisted
};

/** KnownPluginList::CustomScanner that loads each plugin file in a worker process, so a plugin that crashes or hangs
    while it is scanned never takes this process down. One worker is reused across files (and formats, until
    stopWorker()); when a file crashes it, or it has not answered within timeoutMs (the worker is killed), a fresh
    worker is launched and that file is tried once more there (some plugins only crash after others were loaded into
    the same process; a first launch can be slow). A file that fails the fresh worker too is reported failed -> the
    KnownPluginList blacklists it. If a worker cannot be launched at all, every file from then on is reported as empty
    (not failed) and launchError() says why. The worker reports its pid, which is polled for liveness, so a crash is
    noticed within kPollMs. Any thread. Set AMP_SCAN_TRACE=1 in the environment for a line per step on stderr. */
class OutOfProcessScanner final : public juce::KnownPluginList::CustomScanner
{
public:
    explicit OutOfProcessScanner(ScanWorker);
    ~OutOfProcessScanner() override;

    /** Polled while waiting for the worker: true aborts the current file (reported as empty, not failed) and
        kills the worker. CustomScanner::shouldExit() only knows ThreadPool jobs, so the scanning thread passes
        its own check here. */
    void setShouldExit(std::function<bool()>);

    bool findPluginTypesFor(juce::AudioPluginFormat&, juce::OwnedArray<juce::PluginDescription>&,
                            const juce::String& fileOrIdentifier) override;
    void scanFinished() override {} // called per format by PluginDirectoryScanner; the worker stays for the next one
    /** Kills the worker (the scan is over). */
    void stopWorker();

    juce::String launchError() const { return error; }

private:
    class Worker;
    enum class Outcome { Result, Lost, Timeout, Aborted };
    bool launchWorker();
    Outcome ask(const juce::MemoryBlock& request, int timeoutMs, std::unique_ptr<juce::XmlElement>& reply);
    void discardWorker();

    const ScanWorker config;
    std::unique_ptr<Worker> worker;
    std::function<bool()> exitCheck;
    juce::String error;
};

/** The worker side: receives (format name, file) requests over the pipe, loads the plugin with its own
    AudioPluginFormatManager and answers with the descriptions as XML. Exits (stops the message loop) when the
    coordinator disconnects or dies. A message loop must be running in the process. */
class PluginScannerWorker final : private juce::ChildProcessWorker, private juce::AsyncUpdater
{
public:
    using AddFormats = std::function<void(juce::AudioPluginFormatManager&)>;

    static bool isWorkerCommandLine(const juce::String& commandLine);
    /** Non-null when commandLine is a worker launch and the pipe connected. addFormats registers the formats to
        scan with (default: JUCE's default formats, i.e. what the app hosts). */
    static std::unique_ptr<PluginScannerWorker> startIfRequested(const juce::String& commandLine, AddFormats = {});
    ~PluginScannerWorker() override;

    /** False once the coordinator disconnected or died (the message loop has been told to stop by then; a process
        without an NSApplication-style loop can poll this instead). */
    bool isConnected() const { return connected.load(); }

private:
    explicit PluginScannerWorker(AddFormats);
    void handleMessageFromCoordinator(const juce::MemoryBlock&) override;
    void handleConnectionLost() override;
    void handleAsyncUpdate() override;
    juce::OwnedArray<juce::PluginDescription> scan(const juce::MemoryBlock&, bool onMessageThread);
    void reply(const juce::OwnedArray<juce::PluginDescription>&);

    std::mutex mutex;
    std::queue<juce::MemoryBlock> pending;
    juce::AudioPluginFormatManager formats;
    std::atomic<bool> connected { true };
};
} // namespace amp
