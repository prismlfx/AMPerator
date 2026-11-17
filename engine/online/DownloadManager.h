#pragma once

// Download jobs for TONE3000 models: model + tone lookup, streamed fetch, content validation, sha256 and the tone
// image cache, on a small thread pool. Public methods and every onEvent call are on the message thread; workers post
// their events with callAsync, guarded so nothing is delivered after destruction.

#include <atomic>
#include <functional>
#include <map>
#include <memory>

#include <juce_core/juce_core.h>

#include "Tone3000Client.h"
#include "Tone3000Types.h"

namespace amp::online
{
struct DownloadRequest
{
    juce::String jobId, modelId, toneId /* optional */, nodeId /* optional */;
    juce::var addNode; // {type, index} or void; carried through untouched
    juce::String expectedSha; // optional, from the rig
    juce::String rigId;       // optional: the rig the node target belongs to; carried through untouched
};

enum class JobState
{
    Queued,
    Downloading,
    Verifying,
    Done,
    Failed
};

struct JobEvent
{
    juce::String jobId;
    JobState state = JobState::Queued;
    double progress = 0;
    juce::String error;
    // Done only:
    juce::File file; // the partial file; the service moves it into the library
    juce::String sha256;
    ModelInfo model;
    Tone tone;
    juce::File image; // downloaded (or already cached) tone image, or empty
    DownloadRequest request;
};

// {jobId, modelId, toneId, nodeId?, state, progress, file?, sha256?, error?}
juce::var toVar(const JobEvent&);

class DownloadManager
{
public:
    DownloadManager(Tone3000Client&, juce::File partialDir, juce::File artDir /* <libraryRoot>/Art */);
    ~DownloadManager();

    std::function<void(const JobEvent&)> onEvent; // always on the message thread

    juce::String start(DownloadRequest); // returns the jobId (assigned when empty); Queued is emitted asynchronously
    void cancel(const juce::String& jobId); // unknown or finished job: no-op; otherwise one Failed "canceled"
    void cancelAll();                        // blocks until the workers have stopped

    static void cleanPartials(const juce::File& partialDir);

private:
    struct Shared;
    class Job;

    void deliver(const JobEvent&);

    Tone3000Client& client;
    juce::File partialDir, artDir;
    std::shared_ptr<Shared> shared;
    std::map<juce::String, std::shared_ptr<std::atomic<bool>>> active; // jobId -> cancel flag (message thread)
    juce::ThreadPool pool;
};
} // namespace amp::online
