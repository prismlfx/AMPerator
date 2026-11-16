#include "DownloadManager.h"

#include <condition_variable>
#include <mutex>

#include <juce_events/juce_events.h>

#include "Placement.h"
#include "assets/AssetIndex.h"

namespace amp::online
{
namespace
{
const char* const imageExtensions[] = { "jpg", "jpeg", "png", "webp", "gif" };

const char* stateName(JobState s)
{
    switch (s)
    {
        case JobState::Queued: return "queued";
        case JobState::Downloading: return "downloading";
        case JobState::Verifying: return "verifying";
        case JobState::Done: return "done";
        case JobState::Failed: return "failed";
    }
    return "failed";
}

juce::String sanitizeId(const juce::String& id)
{
    juce::String out;
    for (auto c : id)
    {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        out << (ok ? juce::String::charToString(c) : juce::String("_"));
    }
    return out;
}

// Lower-cased extension of the URL's path when it is a known image type, else "jpg".
juce::String imageExtension(const juce::String& url)
{
    auto path = url.upToFirstOccurrenceOf("#", false, false).upToFirstOccurrenceOf("?", false, false);
    const auto lastSegment = path.fromLastOccurrenceOf("/", false, false);
    if (lastSegment.containsChar('.'))
    {
        const auto ext = lastSegment.fromLastOccurrenceOf(".", false, false).toLowerCase();
        for (auto known : imageExtensions)
            if (ext == known)
                return ext;
    }
    return "jpg";
}

juce::String errorText(const ClientError& e)
{
    if (e.kind == ClientError::Kind::NotFound || e.kind == ClientError::Kind::Forbidden)
        return "gone";
    return e.message;
}
} // namespace

struct DownloadManager::Shared
{
    std::atomic<bool> alive { true };
    std::mutex lock;
    std::condition_variable idle;
    int running = 0;
};

class DownloadManager::Job final : public juce::ThreadPoolJob
{
public:
    Job(DownloadManager& m, std::shared_ptr<Shared> s, std::shared_ptr<std::atomic<bool>> c, DownloadRequest r)
        : juce::ThreadPoolJob("online_download"), owner(m), shared(std::move(s)), cancelled(std::move(c)),
          request(std::move(r))
    {
    }

    JobStatus runJob() override
    {
        run();
        {
            std::lock_guard<std::mutex> guard(shared->lock);
            --shared->running;
        }
        shared->idle.notify_all();
        return jobHasFinished;
    }

private:
    bool canceled() const { return cancelled->load(); }

    void post(JobEvent e)
    {
        e.jobId = request.jobId;
        e.request = request;
        auto s = shared;
        auto* self = &owner;
        juce::MessageManager::callAsync([s, self, event = std::move(e)] {
            if (s->alive.load())
                self->deliver(event);
        });
    }

    void fail(const juce::File& partial, const juce::String& error)
    {
        partial.deleteFile();
        JobEvent e;
        e.state = JobState::Failed;
        e.error = canceled() ? juce::String("canceled") : error;
        post(std::move(e));
    }

    // Saves the tone image when it is not cached yet. Returns the cached file or an empty File; never throws a job
    // failure (a cancel is detected by the caller).
    juce::File fetchImage(const Tone& tone, const juce::String& toneId)
    {
        if (tone.images.isEmpty() || toneId.isEmpty())
            return {};
        const auto dir = owner.artDir.getChildFile("tone3000");
        const auto stem = sanitizeId(toneId);
        for (auto ext : imageExtensions)
        {
            auto existing = dir.getChildFile(stem + "." + ext);
            if (existing.existsAsFile())
                return existing;
        }

        const auto& url = tone.images[0];
        const auto target = dir.getChildFile(stem + "." + imageExtension(url));
        const auto temp = dir.getChildFile(stem + ".part");
        dir.createDirectory();
        temp.deleteFile();
        const auto error = owner.client.fetch(url, temp, nullptr, [this] { return canceled(); });
        if (error || canceled() || ! temp.existsAsFile() || temp.getSize() == 0 || ! temp.moveFileTo(target))
        {
            temp.deleteFile();
            return {};
        }
        return target;
    }

    void run()
    {
        const auto partial = owner.partialDir.getChildFile(request.jobId);
        if (canceled())
            return fail(partial, "canceled");

        auto model = owner.client.model(request.modelId);
        if (model.error || ! model.value)
            return fail(partial, errorText(model.error));
        if (canceled())
            return fail(partial, "canceled");

        auto tone = owner.client.tone(model.value->toneId);
        if (tone.error || ! tone.value)
            return fail(partial, errorText(tone.error));
        if (canceled())
            return fail(partial, "canceled");

        owner.partialDir.createDirectory();
        {
            JobEvent e;
            e.state = JobState::Downloading;
            post(std::move(e));
        }

        juce::uint32 lastProgress = juce::Time::getMillisecondCounter();
        auto progress = [this, &lastProgress](juce::int64 received, juce::int64 total) {
            const auto now = juce::Time::getMillisecondCounter();
            if (now - lastProgress < 250)
                return;
            lastProgress = now;
            JobEvent e;
            e.state = JobState::Downloading;
            e.progress = total > 0 ? juce::jlimit(0.0, 1.0, (double) received / (double) total) : 0.0;
            post(std::move(e));
        };

        const auto fetchError = owner.client.fetch(model.value->modelUrl, partial, progress, [this] { return canceled(); });
        if (canceled())
            return fail(partial, "canceled");
        if (fetchError)
            return fail(partial, errorText(fetchError));

        {
            JobEvent e;
            e.state = JobState::Verifying;
            e.progress = 1;
            post(std::move(e));
        }
        const auto invalid = validateAssetFile(partial, tone.value->format);
        if (invalid.isNotEmpty())
            return fail(partial, invalid);

        const auto sha = AssetIndex::sha256Of(partial);
        const auto image = fetchImage(*tone.value, model.value->toneId);
        if (canceled())
            return fail(partial, "canceled");

        JobEvent e;
        e.state = JobState::Done;
        e.progress = 1;
        e.file = partial;
        e.sha256 = sha;
        e.model = *model.value;
        e.tone = *tone.value;
        e.image = image;
        post(std::move(e));
    }

    DownloadManager& owner;
    std::shared_ptr<Shared> shared;
    std::shared_ptr<std::atomic<bool>> cancelled;
    DownloadRequest request;
};

juce::var toVar(const JobEvent& e)
{
    auto* o = new juce::DynamicObject();
    o->setProperty("jobId", e.jobId);
    o->setProperty("modelId", e.request.modelId);
    o->setProperty("toneId", e.request.toneId.isNotEmpty() ? e.request.toneId : e.tone.id);
    if (e.request.nodeId.isNotEmpty())
        o->setProperty("nodeId", e.request.nodeId);
    o->setProperty("state", juce::String(stateName(e.state)));
    o->setProperty("progress", e.progress);
    if (e.state == JobState::Done)
    {
        o->setProperty("file", e.file.getFullPathName());
        o->setProperty("sha256", e.sha256);
    }
    if (e.state == JobState::Failed)
        o->setProperty("error", e.error);
    return juce::var(o);
}

DownloadManager::DownloadManager(Tone3000Client& c, juce::File partials, juce::File art)
    : client(c), partialDir(std::move(partials)), artDir(std::move(art)), shared(std::make_shared<Shared>()), pool(2)
{
}

DownloadManager::~DownloadManager()
{
    shared->alive = false; // events already queued on the message loop are dropped
    cancelAll();
}

juce::String DownloadManager::start(DownloadRequest request)
{
    if (request.jobId.isEmpty())
        request.jobId = juce::Uuid().toString();

    auto flag = std::make_shared<std::atomic<bool>>(false);
    active[request.jobId] = flag;
    {
        std::lock_guard<std::mutex> guard(shared->lock);
        ++shared->running;
    }

    JobEvent queued;
    queued.jobId = request.jobId;
    queued.state = JobState::Queued;
    queued.request = request;
    auto s = shared;
    juce::MessageManager::callAsync([s, this, queued] {
        if (s->alive.load())
            deliver(queued);
    });

    const auto id = request.jobId;
    pool.addJob(new Job(*this, shared, flag, std::move(request)), true);
    return id;
}

void DownloadManager::cancel(const juce::String& jobId)
{
    auto it = active.find(jobId);
    if (it != active.end())
        it->second->store(true);
}

void DownloadManager::cancelAll()
{
    for (auto& entry : active)
        entry.second->store(true);
    std::unique_lock<std::mutex> guard(shared->lock);
    shared->idle.wait(guard, [this] { return shared->running == 0; });
}

void DownloadManager::cleanPartials(const juce::File& dir)
{
    if (! dir.isDirectory())
        return;
    for (const auto& child : dir.findChildFiles(juce::File::findFilesAndDirectories, false))
        child.deleteRecursively();
}

void DownloadManager::deliver(const JobEvent& e)
{
    if (e.state == JobState::Done || e.state == JobState::Failed)
        active.erase(e.jobId);
    if (onEvent)
        onEvent(e);
}
} // namespace amp::online
