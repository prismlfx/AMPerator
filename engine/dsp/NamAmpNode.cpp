#include "dsp/NamAmpNode.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <vector>

#include "dsp/ParamRanges.h"

#include "NAM/dsp.h"
#include "NAM/get_dsp.h"

namespace amp
{
namespace
{
constexpr double kDefaultModelRate = 48000.0;
constexpr int kLoadPrewarmBlock = 2048; // prewarm size used at load; prepare() resets for the real block size
constexpr float kNormalizeTargetDb = -18.0f;

std::filesystem::path toPath(const juce::File& f)
{
#if JUCE_WINDOWS
    return std::filesystem::path(f.getFullPathName().toWideCharPointer());
#else
    return std::filesystem::path(f.getFullPathName().toStdString());
#endif
}

// Streaming sample-rate converter: 4-point (cubic) Lagrange interpolation with an explicit fractional read
// position, so it accepts any number of input samples per call and emits however many outputs they allow.
// Latency is ~2 input samples. No allocation after prepare().
class StreamResampler
{
public:
    void prepare(double inRate, double outRate, int maxInput)
    {
        step = inRate / outRate;
        buf.assign((size_t) maxInput + 8, 0.0f);
        reset();
    }

    void reset()
    {
        std::fill(buf.begin(), buf.end(), 0.0f);
        size = 2;  // buf[0], buf[1]: zero history so the first interpolation has a left neighbour
        pos = 1.0; // read position (in input samples) within buf
    }

    // Upper bound on outputs produced by one process() call with `numInput` samples.
    int maxOutput(int numInput) const { return (int) std::ceil((numInput + 4) / step) + 2; }

    int process(const float* in, int numInput, float* out)
    {
        jassert(size + numInput <= (int) buf.size());
        std::copy(in, in + numInput, buf.data() + size);
        size += numInput;

        int produced = 0;
        for (;;)
        {
            const int i = (int) pos;
            if (i + 2 >= size)
                break;
            out[produced++] = interpolate(buf.data() + i, (float) (pos - i));
            pos += step;
        }

        // Keep buf[floor(pos) - 1 ...] for the next call.
        const int keepFrom = juce::jmin((int) pos - 1, size);
        std::copy(buf.data() + keepFrom, buf.data() + size, buf.data());
        size -= keepFrom;
        pos -= keepFrom;
        return produced;
    }

private:
    // Lagrange polynomial through x[-1], x[0], x[1], x[2], evaluated at t in [0, 1).
    static float interpolate(const float* x, float t)
    {
        const float tm1 = t - 1.0f, tm2 = t - 2.0f, tp1 = t + 1.0f;
        return x[-1] * (-t * tm1 * tm2 / 6.0f) + x[0] * (tp1 * tm1 * tm2 / 2.0f)
               + x[1] * (-tp1 * t * tm2 / 2.0f) + x[2] * (tp1 * t * tm1 / 6.0f);
    }

    std::vector<float> buf;
    int size = 0;
    double pos = 1.0, step = 1.0;
};
} // namespace

struct NamAmpNode::Loaded::Data
{
    std::unique_ptr<nam::DSP> model;
    double sampleRate = kDefaultModelRate;
    bool hasLoudness = false;
    double loudness = 0.0;
};

NamAmpNode::Loaded::Loaded(std::unique_ptr<Data> d) : data(std::move(d)) {}
NamAmpNode::Loaded::~Loaded() = default;

struct NamAmpNode::Impl
{
    std::unique_ptr<Loaded> loaded;
    ProcessSpec spec;
    bool prepared = false;

    std::atomic<float> inputGainDb { 0.0f }, outputGainDb { 0.0f }, normalize { 1.0f };
    juce::SmoothedValue<float> inGain { 1.0f }, outGain { 1.0f };
    float normalizeDb = 0.0f; // (-18 - loudness) when the model has loudness metadata

    bool resampling = false;
    int maxModelBlock = 0;
    StreamResampler up, down;          // host -> model, model -> host
    std::vector<float> hostIn, upOut, downIn;
    std::vector<double> modelIn, modelOut;
    std::vector<float> fifo;           // model output already at host rate, waiting to be read
    int fifoCount = 0;
    std::atomic<int> underruns { 0 };

    nam::DSP* model() const { return loaded != nullptr ? loaded->data->model.get() : nullptr; }

    float targetOutDb() const
    {
        return outputGainDb.load() + (normalize.load() >= 0.5f ? normalizeDb : 0.0f);
    }

    // Off the audio thread: size everything for the current spec and model.
    void configure()
    {
        underruns.store(0, std::memory_order_relaxed);
        inGain.reset(spec.sampleRate, 0.02);
        outGain.reset(spec.sampleRate, 0.02);
        inGain.setCurrentAndTargetValue(juce::Decibels::decibelsToGain(inputGainDb.load()));
        outGain.setCurrentAndTargetValue(juce::Decibels::decibelsToGain(targetOutDb()));

        auto* m = model();
        if (m == nullptr)
            return;

        const double modelRate = loaded->data->sampleRate;
        const int maxBlock = juce::jmax(1, spec.maxBlock);
        resampling = std::abs(spec.sampleRate - modelRate) > 1e-6;
        hostIn.assign((size_t) maxBlock, 0.0f);

        if (! resampling)
        {
            maxModelBlock = maxBlock;
        }
        else
        {
            up.prepare(spec.sampleRate, modelRate, maxBlock);
            maxModelBlock = up.maxOutput(maxBlock) + 16; // generous: the model never sees more than this
            down.prepare(modelRate, spec.sampleRate, maxModelBlock);
            upOut.assign((size_t) maxModelBlock, 0.0f);
            downIn.assign((size_t) maxModelBlock, 0.0f);

            // Prime the output FIFO with enough silence to cover the resamplers' lookahead and per-block
            // rounding, so a read of n samples never underruns. ~0.2 ms at 44.1/48 kHz.
            const int prime = (int) std::ceil(3.0 * spec.sampleRate / modelRate) + 4;
            fifo.assign((size_t) (prime + maxBlock + down.maxOutput(maxModelBlock) + 16), 0.0f);
            fifoCount = prime;
        }
        modelIn.assign((size_t) maxModelBlock, 0.0);
        modelOut.assign((size_t) maxModelBlock, 0.0);
        m->Reset(modelRate, maxModelBlock); // also prewarms
    }

    void runModel(const float* in, float* out, int n)
    {
        for (int i = 0; i < n; ++i)
            modelIn[(size_t) i] = in[i];
        double* inPtr = modelIn.data();
        double* outPtr = modelOut.data();
        model()->process(&inPtr, &outPtr, n);
        for (int i = 0; i < n; ++i)
            out[i] = (float) modelOut[(size_t) i];
    }

    void processChunk(float* ch, int n)
    {
        float* in = hostIn.data();
        for (int i = 0; i < n; ++i)
            in[i] = ch[i] * inGain.getNextValue();

        if (! resampling)
        {
            runModel(in, ch, n);
        }
        else
        {
            const int k = up.process(in, n, upOut.data());
            runModel(upOut.data(), downIn.data(), k);
            fifoCount += down.process(downIn.data(), k, fifo.data() + fifoCount);

            const int avail = juce::jmin(n, fifoCount);
            if (avail < n) // the prime should make this impossible; a gap would be an audible click
            {
                underruns.fetch_add(1, std::memory_order_relaxed);
                jassertfalse;
            }
            std::copy(fifo.data(), fifo.data() + avail, ch);
            std::fill(ch + avail, ch + n, 0.0f);
            std::copy(fifo.data() + avail, fifo.data() + fifoCount, fifo.data());
            fifoCount -= avail;
        }

        for (int i = 0; i < n; ++i)
            ch[i] *= outGain.getNextValue();
    }
};

NamAmpNode::NamAmpNode() : impl(std::make_unique<Impl>()) {}
NamAmpNode::~NamAmpNode() = default;

std::unique_ptr<NamAmpNode::Loaded> NamAmpNode::loadModel(const juce::File& file, juce::String& error)
{
    error.clear();
    if (! file.existsAsFile())
    {
        error = "Model file not found: " + file.getFullPathName();
        return nullptr;
    }

    try
    {
        auto data = std::make_unique<Loaded::Data>();
        data->model = nam::get_dsp(toPath(file));
        if (data->model == nullptr)
        {
            error = "Could not load model: " + file.getFileName();
            return nullptr;
        }
        if (data->model->NumInputChannels() != 1 || data->model->NumOutputChannels() != 1)
        {
            error = "Only mono-in/mono-out models are supported: " + file.getFileName();
            return nullptr;
        }

        const double rate = data->model->GetExpectedSampleRate();
        data->sampleRate = rate > 0.0 ? rate : kDefaultModelRate;
        data->hasLoudness = data->model->HasLoudness();
        data->loudness = data->hasLoudness ? data->model->GetLoudness() : 0.0;
        data->model->Reset(data->sampleRate, kLoadPrewarmBlock); // prewarms
        return std::make_unique<Loaded>(std::move(data));
    }
    catch (const std::exception& e)
    {
        error = "Could not load model " + file.getFileName() + ": " + juce::String(e.what());
    }
    catch (...)
    {
        error = "Could not load model " + file.getFileName();
    }
    return nullptr;
}

void NamAmpNode::setModel(std::unique_ptr<Loaded> m)
{
    impl->loaded = std::move(m);
    const auto* d = impl->loaded != nullptr ? impl->loaded->data.get() : nullptr;
    impl->normalizeDb = (d != nullptr && d->hasLoudness) ? kNormalizeTargetDb - (float) d->loudness : 0.0f;
    if (impl->prepared)
        impl->configure();
}

double NamAmpNode::modelSampleRate() const
{
    return impl->loaded != nullptr ? impl->loaded->data->sampleRate : kDefaultModelRate;
}

int NamAmpNode::underrunCount() const { return impl->underruns.load(std::memory_order_relaxed); }

void NamAmpNode::prepare(const ProcessSpec& s)
{
    impl->spec = s;
    impl->prepared = true;
    impl->configure();
}

void NamAmpNode::process(juce::AudioBuffer<float>& io, int numSamples)
{
    if (impl->model() == nullptr || ! impl->prepared)
        return; // passthrough

    impl->inGain.setTargetValue(juce::Decibels::decibelsToGain(impl->inputGainDb.load()));
    impl->outGain.setTargetValue(juce::Decibels::decibelsToGain(impl->targetOutDb()));

    float* ch = io.getWritePointer(0);
    const int maxBlock = juce::jmax(1, impl->spec.maxBlock);
    for (int pos = 0; pos < numSamples; pos += maxBlock)
        impl->processChunk(ch + pos, juce::jmin(maxBlock, numSamples - pos));
}

void NamAmpNode::reset()
{
    if (impl->prepared)
        impl->configure(); // fresh resamplers, output FIFO and model state (Reset() prewarms; allocates)
}

void NamAmpNode::setParam(const juce::String& name, float v)
{
    const auto c = clampParam("NamAmp", name, v);
    if (! c)
        return;
    if (name == "inputGain") impl->inputGainDb.store(*c);
    else if (name == "outputGain") impl->outputGainDb.store(*c);
    else if (name == "normalize") impl->normalize.store(*c >= 0.5f ? 1.0f : 0.0f);
}

juce::NamedValueSet NamAmpNode::getParams() const
{
    juce::NamedValueSet p;
    p.set("inputGain", impl->inputGainDb.load());
    p.set("outputGain", impl->outputGainDb.load());
    p.set("normalize", impl->normalize.load());
    return p;
}

std::unique_ptr<NamAmpNode> makeNamAmp() { return std::make_unique<NamAmpNode>(); }
} // namespace amp
