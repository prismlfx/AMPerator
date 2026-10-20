// Offline renders of reference rigs through the real RigLoader/ProcessingPlan guard against DSP regressions.
// Run with AMP_UPDATE_REFERENCES=1 to (re)write tests/fixtures/reference/*.wav instead of comparing.
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdio>
#include <map>

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>
#include <juce_events/juce_events.h>

#include "dsp/CabIRNode.h"
#include "storage/RigLoader.h"

#include "support/FakePlugins.h"

namespace
{
constexpr double kSampleRate = 48000.0;
constexpr int kBlock = 128;
constexpr double kMaxDiffDb = -80.0; // RMS(output - reference) relative to RMS(reference)

juce::File fixtures() { return juce::File(AMP_FIXTURES_DIR); }

bool updateMode() { return juce::SystemStats::getEnvironmentVariable("AMP_UPDATE_REFERENCES", {}) == "1"; }

juce::AudioBuffer<float> readWav(const juce::File& f)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> r(fm.createReaderFor(f));
    REQUIRE(r != nullptr);
    REQUIRE(r->sampleRate == kSampleRate);
    juce::AudioBuffer<float> b(1, (int) r->lengthInSamples);
    REQUIRE(r->read(&b, 0, b.getNumSamples(), 0, true, false));
    return b;
}

void writeWav24(const juce::File& f, const juce::AudioBuffer<float>& b)
{
    f.getParentDirectory().createDirectory();
    f.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::FileOutputStream> os(f.createOutputStream());
    REQUIRE(os != nullptr);
    std::unique_ptr<juce::AudioFormatWriter> w(wav.createWriterFor(os.get(), kSampleRate, 1, 24, {}, 0));
    REQUIRE(w != nullptr);
    os.release(); // the writer owns the stream
    REQUIRE(w->writeFromAudioSampleBuffer(b, 0, b.getNumSamples()));
}

double rmsOf(const float* x, int n)
{
    double s = 0;
    for (int i = 0; i < n; ++i)
        s += (double) x[i] * x[i];
    return std::sqrt(s / juce::jmax(1, n));
}

double toDb(double lin) { return 20.0 * std::log10(juce::jmax(lin, 1e-12)); }

// Fraction of spectral energy above `hz`.
double energyFractionAbove(const juce::AudioBuffer<float>& b, double hz)
{
    const int order = 18; // 262144 >= 5 s at 48 kHz
    const int size = 1 << order;
    REQUIRE(b.getNumSamples() <= size);
    juce::dsp::FFT fft(order);
    std::vector<float> data((size_t) size * 2, 0.0f);
    std::copy(b.getReadPointer(0), b.getReadPointer(0) + b.getNumSamples(), data.begin());
    fft.performRealOnlyForwardTransform(data.data(), true);
    double lo = 0, hi = 0;
    for (int k = 0; k <= size / 2; ++k)
    {
        const double e = (double) data[(size_t) 2 * k] * data[(size_t) 2 * k] + (double) data[(size_t) 2 * k + 1] * data[(size_t) 2 * k + 1];
        (k * kSampleRate / size >= hz ? hi : lo) += e;
    }
    return hi / (hi + lo);
}

// Builds the rig and renders the DI through its plan, single-threaded, fixed 128-sample blocks.
juce::AudioBuffer<float> renderRig(const juce::String& rigFile, const juce::AudioBuffer<float>& di)
{
    const juce::ScopedJuceInitialiser_GUI juce; // this thread acts as the message thread
    const auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                         .getChildFile("amp_regression_" + juce::Uuid().toString());
    dir.getChildFile("plugins").createDirectory();
    struct Cleanup { juce::File d; ~Cleanup() { d.deleteRecursively(); } } cleanup { dir };

    amp::AssetIndex index;
    index.setRoots({ fixtures().getChildFile("assets") });
    index.rescan();
    auto host = amp_test::makeFakePluginHost(dir.getChildFile("plugins"));
    host->scan([](float) {});
    amp::RigLoader loader(index, *host);

    auto parsed = amp::parseRig(fixtures().getChildFile("rigs").getChildFile(rigFile).loadFileAsString());
    REQUIRE(parsed.rig.has_value());
    juce::String error;
    auto live = loader.build(*parsed.rig, { kSampleRate, kBlock }, error);
    REQUIRE(live.has_value());
    // All assets must resolve by hash: a regression rig that silently degrades to placeholders is useless.
    REQUIRE(live->report.missingAssets.isEmpty());
    REQUIRE(live->report.placeholderNodeIds.isEmpty());
    REQUIRE(live->report.resolvedByFilename.isEmpty());

    // CabIR loads its IR on a background thread; wait for it before the measured render so the output does
    // not depend on timing. Only the cab nodes see the (silent) wait blocks, so no other state is perturbed.
    juce::AudioBuffer<float> work(2, kBlock);
    for (const auto& pn : live->plan->nodes())
        if (auto* cab = dynamic_cast<amp::CabIRNode*>(pn->node.get()))
        {
            const auto deadline = juce::Time::getMillisecondCounter() + 5000;
            while (! cab->irActive())
            {
                REQUIRE(juce::Time::getMillisecondCounter() < deadline);
                work.clear();
                cab->process(work, kBlock);
                juce::Thread::sleep(2);
            }
            for (int done = 0; done < kSampleRate / 2; done += kBlock) // let the swap-in crossfade finish
            {
                work.clear();
                cab->process(work, kBlock);
            }
        }

    const int total = di.getNumSamples();
    juce::AudioBuffer<float> out(1, total);
    for (int pos = 0; pos < total; pos += kBlock)
    {
        const int n = juce::jmin(kBlock, total - pos);
        work.clear();
        work.copyFrom(0, 0, di, 0, pos, n);
        live->plan->process(work, n);
        out.copyFrom(0, pos, work, 0, 0, n);
    }
    return out;
}

struct RigCase { const char* rig; const char* reference; bool namRig; };
const RigCase kRigs[] = {
    { "regression_clean_eq_ir.amprig", "regression_clean_eq_ir.wav", false },
    { "regression_nam_ir.amprig", "regression_nam_ir.wav", true },
    { "regression_nam_ir_gate_bypassed.amprig", "regression_nam_ir_gate_bypassed.wav", true },
};
} // namespace

TEST_CASE("audio_regression: reference rigs match their reference renders")
{
    const auto di = readWav(fixtures().getChildFile("di/di_48k.wav"));
    const double diRms = rmsOf(di.getReadPointer(0), di.getNumSamples());
    const double diHf = energyFractionAbove(di, 1000.0);
    std::map<juce::String, juce::AudioBuffer<float>> renders;

    for (const auto& rc : kRigs)
    {
        INFO("rig: " << rc.rig);
        {
            const auto out = renderRig(rc.rig, di);
            const auto refFile = fixtures().getChildFile("reference").getChildFile(rc.reference);

            // Sanity of the render itself (also what makes a freshly written reference trustworthy).
            const float* x = out.getReadPointer(0);
            bool finite = true;
            for (int i = 0; i < out.getNumSamples(); ++i)
                finite = finite && std::isfinite(x[i]);
            REQUIRE(finite);
            const float peak = out.getMagnitude(0, out.getNumSamples());
            const double outRms = rmsOf(x, out.getNumSamples());
            REQUIRE(peak < 1.0f);
            REQUIRE(toDb(outRms) > -40.0);
            REQUIRE(toDb(outRms) < -6.0);

            renders[rc.rig] = out;
            if (updateMode())
            {
                writeWav24(refFile, out);
                std::printf("[reference] %s: peak %.2f dBFS, rms %.2f dBFS (DI rms %.2f), HF>1k %.4f (DI %.4f)\n",
                            rc.reference, toDb(peak), toDb(outRms), toDb(diRms), energyFractionAbove(out, 1000.0), diHf);
                continue; // never compare a render against the reference it just wrote
            }

            REQUIRE(refFile.existsAsFile());
            const auto ref = readWav(refFile);
            REQUIRE(ref.getNumSamples() == out.getNumSamples());

            double diffSq = 0;
            for (int i = 0; i < ref.getNumSamples(); ++i)
            {
                const double d = (double) x[i] - ref.getSample(0, i);
                diffSq += d * d;
            }
            const double diffRms = std::sqrt(diffSq / ref.getNumSamples());
            const double refRms = rmsOf(ref.getReadPointer(0), ref.getNumSamples());
            INFO(rc.rig << ": diff " << toDb(diffRms) << " dBFS, reference " << toDb(refRms) << " dBFS");
            REQUIRE(toDb(diffRms) < toDb(refRms) + kMaxDiffDb);

            // The rig must actually have done something to the DI.
            double sameSq = 0;
            for (int i = 0; i < di.getNumSamples(); ++i)
            {
                const double d = (double) x[i] - di.getSample(0, i);
                sameSq += d * d;
            }
            REQUIRE(std::sqrt(sameSq / di.getNumSamples()) > 1e-3);
            if (rc.namRig)
                REQUIRE(std::abs(energyFractionAbove(out, 1000.0) - diHf) > 1e-3);
        }
    }

    // The gate bypass must matter: the gated and bypassed NAM rigs may not render alike.
    if (renders.count("regression_nam_ir.amprig") && renders.count("regression_nam_ir_gate_bypassed.amprig"))
    {
        const auto& a = renders["regression_nam_ir.amprig"];
        const auto& b = renders["regression_nam_ir_gate_bypassed.amprig"];
        double s = 0;
        for (int i = 0; i < a.getNumSamples(); ++i)
        {
            const double d = (double) a.getSample(0, i) - b.getSample(0, i);
            s += d * d;
        }
        const double gateDiffDb = toDb(std::sqrt(s / a.getNumSamples()));
        INFO("gated vs bypassed NAM rig differ by " << gateDiffDb << " dBFS");
        REQUIRE(gateDiffDb > -50.0);
    }

    if (updateMode())
        FAIL("references regenerated: review them, commit, then rerun without AMP_UPDATE_REFERENCES");
}
