#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <optional>
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
    dev.run(0.05); // past the bypass ramp
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
    // Fade in from silence: out[t] = sin^2(pi/2 * t / 1440), then exactly 1.
    for (int t = 0; t < 1440; ++t)
    {
        const float s = std::sin(juce::MathConstants<float>::halfPi * (float) t / 1440.0f);
        REQUIRE(std::abs(rec[(size_t) t] - s * s) < 1e-5f);
    }
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
    REQUIRE(outPeak <= inPeak);

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
        // Bypass toggles and their ramps are RT-safe too (a re-engaged node is reset off the audio thread).
        if (k == 500)
            engine.setBypass("gate", true);
        if (k == 200 || k == 600)
            engine.setBypass("cab", true);
        if (k == 300 || k == 700)
            engine.setBypass("cab", false);
        if (k == 400)
            engine.setBypass("amp", true);
        if (k == 402) // re-engaged mid-ramp
            engine.setBypass("amp", false);
        if (k == 800)
            dev.in[0][3] = std::nanf(""); // the output guard runs on every callback
    }
    REQUIRE(dev.out[0] != std::vector<float>(kBlock, 0.0f)); // the chain really produced sound
    REQUIRE(amp::RtSafety::violations() == 0);
}

TEST_CASE("realtime_engine: many swaps keep working while retired plans are drained")
{
    // Far more swaps than the retire queue holds (63): fades must keep starting as long as someone drains.
    amp::RealtimeEngine engine;
    engine.prepareToPlay(kRate, kBlock);
    FakeDevice dev(engine, 1, 2);
    dev.fillInput(0, 0.5f);
    constexpr int swaps = 150;
    for (int i = 1; i <= swaps; ++i)
    {
        const float db = i == swaps ? -12.0f : (i % 2 == 0 ? 0.0f : -6.0f);
        REQUIRE(engine.swapPlan(gainPlan(db)));
        dev.run(0.035); // longer than one fade: every swap is actually faded in
        const float level = 20.0f * std::log10(dev.out[0].back() / 0.5f);
        INFO("swap " << i);
        REQUIRE(std::abs(level - db) < 0.01f);
        engine.drainRetired();
        REQUIRE(engine.retiredPending() == 0);
    }
}

TEST_CASE("realtime_engine: drain timer starts lazily when the MessageManager appears later")
{
    std::optional<amp::RealtimeEngine> engine;
    engine.emplace(); // no MessageManager yet: no timer
    REQUIRE_FALSE(engine->drainTimerRunning());
    {
        const juce::ScopedJuceInitialiser_GUI juce; // this thread becomes the message thread
        REQUIRE(engine->swapPlan(gainPlan(0.0f)));
        REQUIRE(engine->drainTimerRunning());
        engine.reset(); // timer must die before the MessageManager does
    }
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

namespace
{
float maxStep(const std::vector<float>& v)
{
    float m = 0.0f;
    for (size_t i = 1; i < v.size(); ++i)
        m = juce::jmax(m, std::abs(v[i] - v[i - 1]));
    return m;
}

// Writes NaN, +inf and a +5.0 sample into its block (a misbehaving plugin or a corrupt model).
struct HostileNode final : amp::ProcessorNode
{
    void prepare(const amp::ProcessSpec&) override {}
    void process(juce::AudioBuffer<float>& io, int n) override
    {
        for (int i = 0; i < n; ++i)
            io.setSample(0, i, i % 3 == 0 ? std::nanf("") : i % 3 == 1 ? INFINITY : 5.0f);
    }
};
} // namespace

TEST_CASE("realtime_engine: a bypass toggle ramps instead of stepping")
{
    amp::RealtimeEngine engine;
    engine.prepareToPlay(kRate, kBlock);
    REQUIRE(engine.swapPlan(gainPlan(-12.0f)));
    FakeDevice dev(engine, 1, 2);
    dev.fillInput(0, 0.5f);
    dev.run(0.1); // past the plan fade-in
    REQUIRE(allEqual(dev.out[0], 0.5f * dbToGain(-12.0f)));

    std::vector<float> rec { dev.out[0].back() }; // the last sample before the toggle
    REQUIRE(engine.setBypass("gain", true));
    dev.run(0.05, &rec);
    CHECK(maxStep(rec) < 0.005f); // an instant switch would step by 0.375
    CHECK(std::abs(rec.back() - 0.5f) < 1e-6f);

    rec = { dev.out[0].back() };
    REQUIRE(engine.setBypass("gain", false));
    dev.run(0.05, &rec);
    CHECK(maxStep(rec) < 0.005f);
    CHECK(std::abs(rec.back() - 0.5f * dbToGain(-12.0f)) < 1e-6f);
}

TEST_CASE("realtime_engine: a re-engaged cab does not replay its pre-bypass tail")
{
    amp::RealtimeEngine engine;
    engine.prepareToPlay(kRate, kBlock);
    auto cab = amp::makeCabIR();
    juce::String err;
    REQUIRE(cab->loadIR(fixtures().getChildFile("assets/ir_48k.wav"), err));
    const amp::CabIRNode* cabNode = cab.get();
    REQUIRE(engine.swapPlan(plan({ planNode("cab", std::move(cab)) })));
    FakeDevice dev(engine, 1, 2);

    juce::Random rng(3);
    const auto noise = [&] {
        for (auto& x : dev.in[0])
            x = rng.nextFloat() - 0.5f;
    };
    const auto deadline = juce::Time::getMillisecondCounter() + 5000;
    while (! cabNode->irActive() && juce::Time::getMillisecondCounter() < deadline)
    {
        noise();
        dev.callback();
        juce::Thread::sleep(2);
    }
    REQUIRE(cabNode->irActive());
    for (int b = 0; b < 400; ++b) // past the IR swap-in crossfade, loud input right up to the bypass
    {
        noise();
        dev.callback();
    }
    REQUIRE(engine.setBypass("cab", true));
    for (int b = 0; b < 20; ++b) // through the bypass ramp, still loud
    {
        noise();
        dev.callback();
    }
    dev.fillInput(0, 0.0f);
    dev.run(0.05); // bypassed and silent
    REQUIRE(allEqual(dev.out[0], 0.0f));

    std::vector<float> rec;
    REQUIRE(engine.setBypass("cab", false));
    dev.run(0.1, &rec);
    float peak = 0.0f;
    for (float x : rec)
        peak = juce::jmax(peak, std::abs(x));
    CHECK(peak < 1e-6f); // without a reset the convolver's history would ring out here
}

TEST_CASE("realtime_engine: the output guard silences non-finite samples and clips to full scale")
{
    amp::RealtimeEngine engine;
    engine.prepareToPlay(kRate, kBlock);
    REQUIRE(engine.swapPlan(plan({ planNode("bad", std::make_unique<HostileNode>()) })));
    FakeDevice dev(engine, 1, 2);
    dev.fillInput(0, 0.1f);
    dev.run(0.1); // past the fade-in
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < kBlock; ++i)
        {
            const float x = dev.out[(size_t) ch][(size_t) i];
            INFO("ch " << ch << " i " << i << " = " << x);
            REQUIRE(std::isfinite(x));
            REQUIRE(x == (i % 3 == 2 ? 1.0f : 0.0f));
        }
}
