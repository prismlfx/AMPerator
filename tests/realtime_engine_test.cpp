#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

#include "dsp/BuiltinNodes.h"
#include "dsp/CabIRNode.h"
#include "dsp/NamAmpNode.h"
#include "engine/ProcessingPlan.h"
#include "engine/RealtimeEngine.h"
#include "engine/RtSafety.h"

namespace
{
constexpr double kRate = 48000.0;
constexpr int kBlock = 128;

juce::File fixtures() { return juce::File(AMP_FIXTURES_DIR); }

std::shared_ptr<amp::PlanNode> planNode(const juce::String& id, std::unique_ptr<amp::ProcessorNode> node)
{
    auto pn = std::make_shared<amp::PlanNode>();
    pn->id = id;
    pn->node = std::move(node);
    pn->node->prepare({ kRate, kBlock });
    return pn;
}

std::shared_ptr<amp::PlanNode> gainNode(const juce::String& id, float db)
{
    auto g = amp::makeOutputGain();
    g->setParam("level", db); // before prepare: the smoother starts at the target
    return planNode(id, std::move(g));
}

std::shared_ptr<amp::ProcessingPlan> plan(std::vector<std::shared_ptr<amp::PlanNode>> nodes, double sr = kRate)
{
    return std::make_shared<amp::ProcessingPlan>(std::move(nodes), amp::ProcessSpec { sr, kBlock });
}

std::shared_ptr<amp::ProcessingPlan> gainPlan(float db) { return plan({ gainNode("gain", db) }); }

float dbToGain(float db) { return std::pow(10.0f, db / 20.0f); }

// Drives the engine like a device would: fixed channel counts, one callback per block.
struct FakeDevice
{
    FakeDevice(amp::RealtimeEngine& e, int numIn, int numOut, int block = kBlock)
        : engine(e), in((size_t) numIn, std::vector<float>((size_t) block)),
          out((size_t) numOut, std::vector<float>((size_t) block)), blockSize(block)
    {
        for (auto& c : in) inPtrs.push_back(c.data());
        for (auto& c : out) outPtrs.push_back(c.data());
    }

    void fillInput(int ch, float value)
    {
        std::fill(in[(size_t) ch].begin(), in[(size_t) ch].end(), value);
    }

    void callback()
    {
        for (auto& c : out) std::fill(c.begin(), c.end(), 123.0f); // garbage: the engine must overwrite all
        engine.audioDeviceIOCallbackWithContext(inPtrs.data(), (int) inPtrs.size(), outPtrs.data(),
                                                (int) outPtrs.size(), blockSize, {});
    }

    // Runs callbacks for `seconds`, appending output channel `ch` to `record` if given.
    void run(double seconds, std::vector<float>* record = nullptr, int ch = 0)
    {
        const int blocks = (int) std::ceil(seconds * kRate / blockSize);
        for (int b = 0; b < blocks; ++b)
        {
            callback();
            if (record != nullptr)
                record->insert(record->end(), out[(size_t) ch].begin(), out[(size_t) ch].end());
        }
    }

    amp::RealtimeEngine& engine;
    std::vector<std::vector<float>> in, out;
    std::vector<const float*> inPtrs;
    std::vector<float*> outPtrs;
    int blockSize;
};

bool allEqual(const std::vector<float>& v, float value, float tol = 1e-6f)
{
    for (float x : v)
        if (std::abs(x - value) > tol)
            return false;
    return true;
}
} // namespace

TEST_CASE("realtime_engine: bypassed node is skipped")
{
    amp::RealtimeEngine engine;
    engine.prepareToPlay(kRate, kBlock);
    REQUIRE(engine.swapPlan(gainPlan(-6.0f)));
    FakeDevice dev(engine, 1, 2);
    dev.fillInput(0, 0.5f);
    dev.run(0.1); // past the fade-in

    REQUIRE(allEqual(dev.out[0], 0.5f * dbToGain(-6.0f)));
    REQUIRE(engine.getBypass("gain") == false);

    REQUIRE(engine.setBypass("gain", true));
    REQUIRE(engine.getBypass("gain") == true);
    dev.callback();
    REQUIRE(allEqual(dev.out[0], 0.5f));
    REQUIRE(allEqual(dev.out[1], 0.5f));

    REQUIRE(engine.setBypass("gain", false));
    dev.run(0.05);
    REQUIRE(allEqual(dev.out[0], 0.5f * dbToGain(-6.0f)));
}

TEST_CASE("realtime_engine: mono input reaches both outputs")
{
    amp::RealtimeEngine engine;
    engine.prepareToPlay(kRate, kBlock);
    REQUIRE(engine.swapPlan(plan({ planNode("eq", amp::makeEQ()) })));
    FakeDevice dev(engine, 1, 2);
    for (int i = 0; i < kBlock; ++i)
        dev.in[0][(size_t) i] = 0.25f * (float) std::sin(0.05 * i);
    dev.run(0.1);
    for (int i = 0; i < kBlock; ++i)
    {
        REQUIRE(std::abs(dev.out[0][(size_t) i] - dev.in[0][(size_t) i]) < 1e-5f); // flat EQ is transparent
        REQUIRE(dev.out[1][(size_t) i] == dev.out[0][(size_t) i]);
    }
}

TEST_CASE("realtime_engine: routes the selected input to the selected output pair")
{
    amp::RealtimeEngine engine;
    engine.setInputChannel(1);
    engine.setOutputPair(2);
    engine.prepareToPlay(kRate, kBlock);
    REQUIRE(engine.swapPlan(plan({ planNode("p", amp::makePlaceholder()) })));

    SECTION("pair inside a 4-output device, other outputs zeroed")
    {
        FakeDevice dev(engine, 2, 4);
        dev.fillInput(0, 0.9f);
        dev.fillInput(1, 0.3f);
        dev.run(0.1);
        REQUIRE(allEqual(dev.out[0], 0.0f));
        REQUIRE(allEqual(dev.out[1], 0.0f));
        REQUIRE(allEqual(dev.out[2], 0.3f));
        REQUIRE(allEqual(dev.out[3], 0.3f));
    }
    SECTION("only the left output exists")
    {
        FakeDevice dev(engine, 2, 3);
        dev.fillInput(1, 0.3f);
        dev.run(0.1);
        REQUIRE(allEqual(dev.out[2], 0.3f));
        REQUIRE(allEqual(dev.out[0], 0.0f));
    }
    SECTION("missing input processes silence")
    {
        FakeDevice dev(engine, 1, 4);
        dev.fillInput(0, 0.9f);
        dev.run(0.1);
        for (const auto& ch : dev.out)
            REQUIRE(allEqual(ch, 0.0f));
    }
}

TEST_CASE("realtime_engine: swap crossfades without discontinuity")
{
    amp::RealtimeEngine engine;
    engine.prepareToPlay(kRate, kBlock);
    REQUIRE(engine.swapPlan(gainPlan(0.0f)));
    FakeDevice dev(engine, 1, 2);
    dev.fillInput(0, 0.5f);
    dev.run(0.1);

    std::vector<float> rec(dev.out[0]);
    REQUIRE(engine.swapPlan(gainPlan(0.0f)));
    dev.run(0.1, &rec);

    float maxJump = 0.0f;
    for (size_t i = 1; i < rec.size(); ++i)
        maxJump = std::max(maxJump, std::abs(rec[i] - rec[i - 1]));
    INFO("max sample-to-sample jump " << maxJump);
    REQUIRE(maxJump < 0.01f);
    REQUIRE(std::abs(rec.back() - 0.5f) < 1e-6f);
    // The new plan really took over (the fade did not stall).
    engine.drainRetired();
    REQUIRE(engine.retiredPending() == 0);
}

TEST_CASE("realtime_engine: crossfade is sample-accurate across block boundaries")
{
    // 30 ms at 48 kHz = 1440 samples, spread over several 100-sample callbacks (not a divisor of 1440).
    amp::RealtimeEngine engine;
    engine.prepareToPlay(kRate, 100);
    FakeDevice dev(engine, 1, 1, 100);
    dev.fillInput(0, 1.0f);
    REQUIRE(engine.swapPlan(plan({ planNode("p", amp::makePlaceholder()) })));
    std::vector<float> rec;
    dev.run(0.05, &rec);
    // Fade in from silence: out[t] = sin(pi/2 * t / 1440), then exactly 1.
    for (int t = 0; t < 1440; ++t)
        REQUIRE(std::abs(rec[(size_t) t] - std::sin(juce::MathConstants<float>::halfPi * (float) t / 1440.0f)) < 1e-5f);
    for (size_t t = 1440; t < rec.size(); ++t)
        REQUIRE(rec[t] == 1.0f);
}

TEST_CASE("realtime_engine: rapid plan swaps settle on last plan")
{
    amp::RealtimeEngine engine;
    engine.prepareToPlay(kRate, kBlock);
    FakeDevice dev(engine, 1, 2);
    constexpr float inPeak = 0.5f;
    dev.fillInput(0, inPeak);

    std::vector<std::weak_ptr<amp::ProcessingPlan>> plans;
    std::vector<float> rec;
    int swaps = 0;
    for (int cb = 0; cb < 50; ++cb)
    {
        for (int k = 0; k < 2; ++k)
        {
            ++swaps;
            const float db = swaps == 100 ? -12.0f : (swaps % 2 == 0 ? 0.0f : -6.0f);
            auto p = gainPlan(db);
            plans.push_back(p);
            REQUIRE(engine.swapPlan(std::move(p)));
        }
        dev.callback();
        rec.insert(rec.end(), dev.out[0].begin(), dev.out[0].end());
    }
    REQUIRE(swaps == 100);
    dev.run(0.1, &rec);

    const float expected = inPeak * dbToGain(-12.0f);
    const float lastDb = 20.0f * std::log10(dev.out[0].back() / inPeak);
    INFO("settled at " << lastDb << " dB");
    REQUIRE(std::abs(lastDb - (-12.0f)) <= 0.1f);
    REQUIRE(allEqual(dev.out[0], expected, 1e-5f));

    float outPeak = 0.0f;
    for (float x : rec)
        outPeak = std::max(outPeak, std::abs(x));
    INFO("output peak " << outPeak << " vs input peak " << inPeak);
    // Every plan has gain <= 0 dB. An equal-power (sin/cos) crossfade between two such plans fed the same
    // signal peaks at sqrt(gA^2 + gB^2) <= sqrt(2) (+3 dB, reached when both plans are 0 dB) mid-fade; the
    // brief's literal "<= input peak" only holds for an equal-gain fade (see task-8 report, concern 1).
    REQUIRE(outPeak <= inPeak * std::sqrt(2.0f) + 1e-6f);

    engine.drainRetired();
    REQUIRE(engine.retiredPending() == 0);
    // Every plan except the live one has been freed (off the audio thread).
    for (size_t i = 0; i + 1 < plans.size(); ++i)
        REQUIRE(plans[i].expired());
    REQUIRE_FALSE(plans.back().expired());
}

TEST_CASE("realtime_engine: callback performs no allocation")
{
    amp::RealtimeEngine engine;
    engine.prepareToPlay(kRate, kBlock);

    juce::String err;
    auto amp = amp::makeNamAmp();
    amp->setModel(amp::NamAmpNode::loadModel(fixtures().getChildFile("assets/tiny.nam"), err));
    REQUIRE(err.isEmpty());
    auto cab = amp::makeCabIR();
    REQUIRE(cab->loadIR(fixtures().getChildFile("assets/ir_48k.wav"), err));
    const amp::CabIRNode* cabNode = cab.get();
    REQUIRE(engine.swapPlan(plan({ planNode("gate", amp::makeGate()), planNode("amp", std::move(amp)),
                                   planNode("cab", std::move(cab)) })));

    FakeDevice dev(engine, 1, 2);
    int cb = 0;
    const auto callback = [&] {
        for (int i = 0; i < kBlock; ++i)
            dev.in[0][(size_t) i] = 0.3f * (float) std::sin(2.0 * juce::MathConstants<double>::pi * 220.0
                                                            * (cb * kBlock + i) / kRate);
        dev.callback();
        ++cb;
    };

    amp::RtSafety::reset();
    // Include the convolver's asynchronous IR swap-in (completed inside process()) in the measurement.
    const auto deadline = juce::Time::getMillisecondCounter() + 5000;
    while (! cabNode->irActive() && juce::Time::getMillisecondCounter() < deadline)
    {
        callback();
        juce::Thread::sleep(2);
    }
    REQUIRE(cabNode->irActive());
    for (int k = 0; k < 1000; ++k)
    {
        callback();
        if (k == 500)
            engine.setBypass("gate", true); // bypass toggles are RT-safe too
    }
    REQUIRE(dev.out[0] != std::vector<float>(kBlock, 0.0f)); // the chain really produced sound
    REQUIRE(amp::RtSafety::violations() == 0);
}

TEST_CASE("realtime_engine: setBypass unknown id returns false")
{
    amp::RealtimeEngine engine;
    REQUIRE_FALSE(engine.setBypass("gain", true)); // no plan at all
    REQUIRE(engine.swapPlan(gainPlan(0.0f)));
    REQUIRE_FALSE(engine.setBypass("nope", true));
    REQUIRE_FALSE(engine.getBypass("nope").has_value());
}

TEST_CASE("realtime_engine: refuses a plan for another sample rate while running")
{
    amp::RealtimeEngine engine;
    engine.prepareToPlay(kRate, kBlock);
    REQUIRE(engine.swapPlan(gainPlan(-6.0f)));
    // A mismatched plan is refused (and jasserts in debug builds) so the running plan keeps playing.
    REQUIRE_FALSE(engine.swapPlan(plan({ gainNode("other", 0.0f) }, 44100.0)));
    REQUIRE(engine.getBypass("gain").has_value());
    FakeDevice dev(engine, 1, 2);
    dev.fillInput(0, 0.5f);
    dev.run(0.1);
    REQUIRE(allEqual(dev.out[0], 0.5f * dbToGain(-6.0f)));
}

TEST_CASE("realtime_engine: restarting at another rate drops mismatched plans")
{
    amp::RealtimeEngine engine;
    engine.prepareToPlay(kRate, kBlock);
    REQUIRE(engine.swapPlan(gainPlan(0.0f)));
    FakeDevice dev(engine, 1, 2);
    dev.fillInput(0, 0.5f);
    dev.run(0.1);
    engine.audioDeviceStopped();
    engine.prepareToPlay(44100.0, kBlock);
    dev.run(0.1);
    REQUIRE(allEqual(dev.out[0], 0.0f));
    REQUIRE_FALSE(engine.getBypass("gain").has_value());
}

TEST_CASE("realtime_engine: meters report peak dBFS since last read")
{
    amp::RealtimeEngine engine;
    REQUIRE(engine.inputPeakDb() == amp::RealtimeEngine::silenceDb);
    engine.prepareToPlay(kRate, kBlock);
    REQUIRE(engine.swapPlan(gainPlan(-6.0f)));
    FakeDevice dev(engine, 1, 2);
    dev.fillInput(0, 0.5f);
    dev.in[0][7] = -1.0f;
    dev.run(0.1);
    REQUIRE(std::abs(engine.inputPeakDb() - 0.0f) < 1e-4f);
    REQUIRE(std::abs(engine.outputPeakDb() - (-6.0f)) < 1e-3f);
    // Read resets; silence floors at -100 dB.
    dev.fillInput(0, 0.0f);
    dev.callback();
    REQUIRE(engine.inputPeakDb() == amp::RealtimeEngine::silenceDb);
    REQUIRE(engine.outputPeakDb() == amp::RealtimeEngine::silenceDb);
}

TEST_CASE("realtime_engine: analyzer tap receives the raw selected input")
{
    amp::RealtimeEngine engine;
    std::vector<float> tapped;
    tapped.reserve(4096);
    engine.analyzerTap = [&tapped](const float* d, int n) { tapped.insert(tapped.end(), d, d + n); };
    engine.setInputChannel(1);
    engine.prepareToPlay(kRate, kBlock);
    REQUIRE(engine.swapPlan(gainPlan(-20.0f)));
    FakeDevice dev(engine, 2, 2);
    dev.fillInput(1, 0.4f);
    dev.callback();
    REQUIRE(tapped.size() == (size_t) kBlock);
    REQUIRE(allEqual(tapped, 0.4f));
}

TEST_CASE("realtime_engine: alloc guard covers the audio callback")
{
    if (! amp::RtSafety::active())
        return; // release builds: nothing to guard
    amp::RealtimeEngine engine;
    engine.analyzerTap = [](const float*, int) { ::operator delete(::operator new(16)); }; // deliberately unsafe
    engine.prepareToPlay(kRate, kBlock);
    FakeDevice dev(engine, 1, 2);
    amp::RtSafety::reset();
    dev.callback();
    REQUIRE(amp::RtSafety::violations() == 2);
    amp::RtSafety::reset();
}

TEST_CASE("realtime_engine: RtSafety counts allocations inside ScopedNoAlloc")
{
    if (! amp::RtSafety::active())
    {
        REQUIRE(amp::RtSafety::violations() == 0); // release builds: no-op guard
        return;
    }
    amp::RtSafety::reset();
    {
        void* outside = ::operator new(64);
        ::operator delete(outside);
    }
    REQUIRE(amp::RtSafety::violations() == 0);
    {
        const amp::RtSafety::ScopedNoAlloc guard;
        void* p = ::operator new(64); // direct calls to the replaceable functions cannot be elided
        ::operator delete(p);
        void* a = ::operator new[](32, std::align_val_t { 64 });
        REQUIRE(reinterpret_cast<std::uintptr_t>(a) % 64 == 0);
        ::operator delete[](a, std::align_val_t { 64 });
    }
    REQUIRE(amp::RtSafety::violations() == 4); // 2 allocations + 2 frees
    amp::RtSafety::reset();
    REQUIRE(amp::RtSafety::violations() == 0);
}
