#include "plugins/PluginScanner.h"

#include <cstdlib>
#include <iostream>

#if JUCE_WINDOWS
 #include <windows.h>
#else
 #include <signal.h>
 #include <sys/wait.h>
 #include <unistd.h>
#endif

namespace amp
{
namespace
{
constexpr int kLaunchTimeoutMs = 15000; // worker process start + hello; also the pipe's silence timeout (a fallback)
constexpr int kPollMs = 50;
constexpr juce::int32 kRequestMagic = 0x414d5053; // "AMPS": JUCE's own start/ping blocks must not look like requests

// Liveness and termination by pid. JUCE's ChildProcessCoordinator keeps its ChildProcess private, and its pipe never
// reports a dead peer promptly (it is opened read/write, so there is no EOF; only the silence timeout notices), so
// the worker tells us its pid and we watch that. The worker is this process's child: waitpid also reaps it.
int currentPid()
{
#if JUCE_WINDOWS
    return (int) GetCurrentProcessId();
#else
    return (int) getpid();
#endif
}

bool processIsAlive(int pid)
{
    if (pid <= 0)
        return false;
#if JUCE_WINDOWS
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, (DWORD) pid);
    if (h == nullptr)
        return false;
    const bool alive = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
    CloseHandle(h);
    return alive;
#else
    int status = 0;
    return waitpid((pid_t) pid, &status, WNOHANG) == 0;
#endif
}

void terminateProcess(int pid)
{
    if (pid <= 0)
        return;
#if JUCE_WINDOWS
    if (HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, (DWORD) pid))
    {
        TerminateProcess(h, 1);
        WaitForSingleObject(h, 2000);
        CloseHandle(h);
    }
#else
    if (::kill((pid_t) pid, SIGKILL) == 0)
    {
        int status = 0;
        waitpid((pid_t) pid, &status, 0); // SIGKILL is prompt; this reaps the zombie
    }
#endif
}

// AMP_SCAN_TRACE=1 traces to stderr; an absolute path appends to that file (the worker's stderr is /dev/null).
void trace(const juce::String& line)
{
    const auto target = juce::SystemStats::getEnvironmentVariable("AMP_SCAN_TRACE", {});
    if (target.isEmpty())
        return;
    const auto text = "[scan " + juce::String(juce::Time::getMillisecondCounter() % 100000) + " pid "
                      + juce::String(currentPid()) + "] " + line;
    if (juce::File::isAbsolutePath(target))
        juce::File(target).appendText(text + "\n");
    else
        std::cerr << text << std::endl;
}

juce::MemoryBlock encodeRequest(const juce::String& formatName, const juce::String& fileOrIdentifier)
{
    juce::MemoryBlock request;
    juce::MemoryOutputStream out(request, true);
    out.writeInt(kRequestMagic);
    out.writeString(formatName);
    out.writeString(fileOrIdentifier);
    return request;
}

// False for anything that is not one of ours (the coordinator's start message, for one).
bool decodeRequest(const juce::MemoryBlock& block, juce::String& formatName, juce::String& fileOrIdentifier)
{
    juce::MemoryInputStream in(block, false);
    if (block.getSize() < sizeof(juce::int32) || in.readInt() != kRequestMagic)
        return false;
    formatName = in.readString();
    fileOrIdentifier = in.readString();
    return true;
}
} // namespace

//==============================================================================
// The coordinator end of one worker process. Replies and the connection-lost notice arrive on JUCE's pipe thread
// (a ping failure on the message thread); wait() hands them to the scanning thread.
class OutOfProcessScanner::Worker final : private juce::ChildProcessCoordinator
{
public:
    ~Worker() override
    {
        terminate();
        killWorkerProcess();
    }

    bool launch(const juce::File& exe) { return launchWorkerProcess(exe, kPluginScannerUid, kLaunchTimeoutMs, 0); }
    bool send(const juce::MemoryBlock& mb) { return sendMessageToWorker(mb); }
    void setPid(int p) { pid = p; }
    /** Kills the process now (hung or no longer wanted); the pipe side is cleaned up by the destructor. */
    void terminate()
    {
        terminateProcess(pid);
        pid = 0;
    }

    enum class State { Waiting, Result, Lost };
    /** Waits up to `ms` for the next reply (xml set, null for an unparsable one), the connection to drop, or the
        process to die. */
    State wait(int ms, std::unique_ptr<juce::XmlElement>& xmlOut)
    {
        std::unique_lock<std::mutex> lock(mutex);
        if (! condvar.wait_for(lock, std::chrono::milliseconds(ms), [&] { return ! replies.empty() || lost; }))
        {
            if (pid > 0 && ! processIsAlive(pid))
            {
                lost = true;
                return State::Lost;
            }
            return State::Waiting;
        }
        if (! replies.empty())
        {
            xmlOut = std::move(replies.front());
            replies.pop();
            return State::Result;
        }
        return State::Lost;
    }

private:
    void handleMessageFromWorker(const juce::MemoryBlock& mb) override
    {
        const std::lock_guard<std::mutex> lock(mutex);
        replies.push(juce::parseXML(mb.toString()));
        condvar.notify_one();
    }
    void handleConnectionLost() override
    {
        const std::lock_guard<std::mutex> lock(mutex);
        lost = true;
        condvar.notify_one();
    }

    std::mutex mutex;
    std::condition_variable condvar;
    std::queue<std::unique_ptr<juce::XmlElement>> replies;
    bool lost = false;
    int pid = 0;
};

OutOfProcessScanner::OutOfProcessScanner(ScanWorker c) : config(std::move(c)) {}
OutOfProcessScanner::~OutOfProcessScanner() { worker.reset(); }

void OutOfProcessScanner::setShouldExit(std::function<bool()> fn) { exitCheck = std::move(fn); }

void OutOfProcessScanner::stopWorker() { discardWorker(); }

void OutOfProcessScanner::discardWorker()
{
    if (worker == nullptr)
        return;
    worker->terminate(); // the process is gone now
    // JUCE may still deliver a ping failure to it on the message thread, so the object itself is deleted there
    // (its pipe threads are stopped by then). On the message thread (a blocking scan), right away.
    if (juce::MessageManager::getInstance()->isThisTheMessageThread())
        worker.reset();
    else
    {
        std::shared_ptr<Worker> dead(std::move(worker));
        juce::MessageManager::callAsync([dead] {});
    }
}

bool OutOfProcessScanner::launchWorker()
{
    discardWorker();
    const auto fail = [&](const juce::String& why)
    {
        discardWorker();
        error = "Could not start the plugin scan worker (" + config.executable.getFullPathName() + "): " + why;
        trace(error);
        return false;
    };
    if (! config.executable.existsAsFile())
        return fail("no such file");
    worker = std::make_unique<Worker>();
    if (! worker->launch(config.executable))
        return fail("launch failed");
    // A process that started but never serves the pipe (exec failed, not a worker build) must not look like a
    // plugin crash: it has to answer a hello (an empty request) first, and that reply names its pid.
    std::unique_ptr<juce::XmlElement> reply;
    switch (ask(encodeRequest({}, {}), kLaunchTimeoutMs, reply))
    {
        case Outcome::Result: break;
        case Outcome::Aborted: discardWorker(); return false; // cancelled, not broken
        case Outcome::Lost:
        case Outcome::Timeout: return fail("no answer");
    }
    const int pid = reply != nullptr ? reply->getIntAttribute("pid") : 0;
    if (pid <= 0)
        return fail("no pid in the hello reply");
    worker->setPid(pid);
    trace("worker up, pid " + juce::String(pid));
    return true;
}

OutOfProcessScanner::Outcome OutOfProcessScanner::ask(const juce::MemoryBlock& request, int timeoutMs,
                                                      std::unique_ptr<juce::XmlElement>& reply)
{
    if (worker == nullptr || ! worker->send(request))
    {
        discardWorker();
        return Outcome::Lost;
    }
    const auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32) juce::jmax(1, timeoutMs);
    for (;;)
    {
        if (exitCheck && exitCheck())
        {
            discardWorker();
            return Outcome::Aborted;
        }
        switch (worker->wait(kPollMs, reply))
        {
            case Worker::State::Result: return Outcome::Result;
            case Worker::State::Lost: discardWorker(); return Outcome::Lost;
            case Worker::State::Waiting: break;
        }
        if (juce::Time::getMillisecondCounter() >= deadline)
        {
            discardWorker(); // kills the hung worker
            return Outcome::Timeout;
        }
    }
}

bool OutOfProcessScanner::findPluginTypesFor(juce::AudioPluginFormat& format,
                                             juce::OwnedArray<juce::PluginDescription>& result,
                                             const juce::String& fileOrIdentifier)
{
    if (error.isNotEmpty())
        return true; // no worker: nothing found, nothing blamed
    const auto request = encodeRequest(format.getName(), fileOrIdentifier);

    // Two attempts; the second always in a freshly launched worker.
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        if (worker == nullptr && ! launchWorker())
            return true;
        std::unique_ptr<juce::XmlElement> reply;
        const auto outcome = ask(request, config.timeoutMs, reply);
        trace(fileOrIdentifier + " attempt " + juce::String(attempt + 1) + ": "
              + (outcome == Outcome::Result ? "result" : outcome == Outcome::Lost ? "worker died"
                 : outcome == Outcome::Timeout ? "timeout, worker killed" : "aborted"));
        switch (outcome)
        {
            case Outcome::Result:
                if (reply != nullptr)
                    for (const auto* item : reply->getChildIterator())
                    {
                        auto desc = std::make_unique<juce::PluginDescription>();
                        if (desc->loadFromXml(*item))
                            result.add(desc.release());
                    }
                return true;
            case Outcome::Lost:
            case Outcome::Timeout: continue; // crashed or hung on this file: once more, in a fresh process
            case Outcome::Aborted: return true; // cancelled: not the plugin's fault
        }
    }
    return false; // failed a fresh worker too
}

//==============================================================================
PluginScannerWorker::PluginScannerWorker(AddFormats addFormats)
{
    if (addFormats)
        addFormats(formats);
    else
        juce::addDefaultFormatsToManager(formats);
}

PluginScannerWorker::~PluginScannerWorker() { cancelPendingUpdate(); }

bool PluginScannerWorker::isWorkerCommandLine(const juce::String& commandLine)
{
    return commandLine.trim().startsWith("--" + juce::String(kPluginScannerUid) + ":");
}

std::unique_ptr<PluginScannerWorker> PluginScannerWorker::startIfRequested(const juce::String& commandLine,
                                                                           AddFormats addFormats)
{
    if (! isWorkerCommandLine(commandLine))
        return nullptr;
    std::unique_ptr<PluginScannerWorker> w(new PluginScannerWorker(std::move(addFormats)));
    if (! w->initialiseFromCommandLine(commandLine, kPluginScannerUid, 0))
        return nullptr;
    return w;
}

void PluginScannerWorker::handleMessageFromCoordinator(const juce::MemoryBlock& mb)
{
    juce::String formatName, identifier;
    if (! decodeRequest(mb, formatName, identifier))
        return;
    const std::lock_guard<std::mutex> lock(mutex);
    trace("worker: request " + formatName + " " + identifier);
    // Formats that must not be loaded on a blocked message thread (AUv3) are scanned right here, on the pipe
    // thread; everything else is handed to the message thread.
    if (auto results = scan(mb, false); ! results.isEmpty())
    {
        reply(results);
        return;
    }
    pending.push(mb);
    triggerAsyncUpdate();
}

void PluginScannerWorker::handleAsyncUpdate()
{
    for (;;)
    {
        const std::lock_guard<std::mutex> lock(mutex);
        if (pending.empty())
            return;
        reply(scan(pending.front(), true));
        pending.pop();
    }
}

void PluginScannerWorker::handleConnectionLost()
{
    trace("worker: connection lost");
    // The coordinator is gone: leave. If a plugin has the message thread hung, the fallback thread still exits.
    connected.store(false);
    juce::MessageManager::callAsync([] { juce::MessageManager::getInstance()->stopDispatchLoop(); });
    juce::Thread::launch([]
    {
        juce::Thread::sleep(3000);
        std::_Exit(0);
    });
}

juce::OwnedArray<juce::PluginDescription> PluginScannerWorker::scan(const juce::MemoryBlock& block, bool onMessageThread)
{
    juce::String formatName, identifier;
    decodeRequest(block, formatName, identifier);

    juce::PluginDescription pd;
    pd.fileOrIdentifier = identifier;
    pd.uniqueId = pd.deprecatedUid = 0;

    juce::OwnedArray<juce::PluginDescription> results;
    for (auto* format : formats.getFormats())
        if (format->getName() == formatName
            && (onMessageThread || format->requiresUnblockedMessageThreadDuringCreation(pd)))
            format->findAllTypesForFile(results, identifier);
    return results;
}

void PluginScannerWorker::reply(const juce::OwnedArray<juce::PluginDescription>& results)
{
    trace("worker: reply with " + juce::String(results.size()) + " plugin(s)");
    juce::XmlElement xml("LIST");
    xml.setAttribute("pid", currentPid()); // the coordinator watches it for liveness
    for (const auto* desc : results)
        xml.addChildElement(desc->createXml().release());
    const auto text = xml.toString();
    sendMessageToCoordinator({ text.toRawUTF8(), text.getNumBytesAsUTF8() });
}
} // namespace amp
