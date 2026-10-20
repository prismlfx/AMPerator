#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "dsp/TunerTap.h"
#include "engine/RtSafety.h"
#include <cmath>
#include <vector>

using namespace amp;

namespace
{
constexpr double kPi = 3.14159265358979323846;

std::vector<float> sine(double hz, double sr, int n, float amp = 0.5f)
{
    std::vector<float> v((size_t) n);
    for (int i = 0; i < n; ++i) v[(size_t) i] = amp * (float) std::sin(2.0 * kPi * hz * i / sr);
    return v;
}

// Band-unlimited-ish saw (additive, harmonics below Nyquist) with a weak fundamental relative to the sum.
std::vector<float> saw(double hz, double sr, int n, float amp = 0.3f)
{
    std::vector<float> v((size_t) n, 0.0f);
    for (int h = 1; h * hz < sr / 2; ++h)
        for (int i = 0; i < n; ++i)
            v[(size_t) i] += amp * (float) (std::sin(2.0 * kPi * hz * h * i / sr) / h);
    return v;
}

void feed(TunerTap& t, const std::vector<float>& s)
{
    for (size_t i = 0; i < s.size(); i += 512)
        t.push(s.data() + i, (int) std::min<size_t>(512, s.size() - i));
}
} // namespace

TEST_CASE("tuner: detects low E")
{
    TunerTap t; t.prepare(48000.0);
    feed(t, sine(82.41, 48000.0, 6000));
    auto r = t.poll();
    REQUIRE(r.has_value());
    CHECK(r->note == "E");
    CHECK(r->octave == 2);
    CHECK(std::abs(r->cents) < 2.0f);
}

TEST_CASE("tuner: detects low E at 44.1 kHz")
{
    TunerTap t; t.prepare(44100.0);
    feed(t, sine(82.41, 44100.0, 6000));
    auto r = t.poll();
    REQUIRE(r.has_value());
    CHECK(r->note == "E");
    CHECK(r->octave == 2);
    CHECK(std::abs(r->cents) < 2.0f);
}

TEST_CASE("tuner: detects low E at 96 kHz via decimation")
{
    TunerTap t; t.prepare(96000.0);
    feed(t, sine(82.41, 96000.0, 12000));
    auto r = t.poll();
    REQUIRE(r.has_value());
    CHECK(r->note == "E");
    CHECK(r->octave == 2);
    CHECK(std::abs(r->cents) < 2.0f);
}

TEST_CASE("tuner: detects A4 sharp")
{
    TunerTap t; t.prepare(48000.0);
    feed(t, sine(445.0, 48000.0, 6000));
    auto r = t.poll();
    REQUIRE(r.has_value());
    CHECK(r->note == "A");
    CHECK(r->octave == 4);
    CHECK_THAT(r->cents, Catch::Matchers::WithinAbs(19.6, 2.0));
}

TEST_CASE("tuner: detects high E with harmonics")
{
    TunerTap t; t.prepare(48000.0);
    feed(t, saw(329.63, 48000.0, 6000));
    auto r = t.poll();
    REQUIRE(r.has_value());
    CHECK(r->note == "E");
    CHECK(r->octave == 4);
    CHECK(std::abs(r->cents) < 3.0f);
}

TEST_CASE("tuner: silence returns nullopt")
{
    TunerTap t; t.prepare(48000.0);
    std::vector<float> z(6000, 0.0f);
    feed(t, z);
    CHECK_FALSE(t.poll().has_value());
}

TEST_CASE("tuner: signal below -50 dBFS returns nullopt")
{
    TunerTap t; t.prepare(48000.0);
    feed(t, sine(110.0, 48000.0, 6000, 0.002f)); // RMS ~ -60 dBFS
    CHECK_FALSE(t.poll().has_value());
}

TEST_CASE("tuner: noise with no period returns nullopt")
{
    TunerTap t; t.prepare(48000.0);
    std::vector<float> n(6000);
    unsigned s = 12345;
    for (auto& v : n) { s = s * 1664525u + 1013904223u; v = ((s >> 8) / (float) (1 << 24) - 0.5f); }
    feed(t, n);
    CHECK_FALSE(t.poll().has_value());
}

TEST_CASE("tuner: fewer than one window pushed returns nullopt")
{
    TunerTap t; t.prepare(48000.0);
    feed(t, sine(110.0, 48000.0, 2047));
    CHECK_FALSE(t.poll().has_value());
    float one = 0.1f; t.push(&one, 1);
    CHECK(t.poll().has_value());
}

TEST_CASE("tuner: poll tracks pitch changes")
{
    TunerTap t; t.prepare(48000.0);
    feed(t, sine(110.0, 48000.0, 5000));
    REQUIRE(t.poll().has_value());
    feed(t, sine(196.0, 48000.0, 3000)); // G3
    auto r = t.poll();
    REQUIRE(r.has_value());
    CHECK(r->note == "G");
    CHECK(r->octave == 3);
}

TEST_CASE("tuner: push allocates nothing")
{
    TunerTap t; t.prepare(48000.0);
    auto s = sine(110.0, 48000.0, 512);
    RtSafety::reset();
    {
        RtSafety::ScopedNoAlloc guard;
        for (int i = 0; i < 40; ++i) t.push(s.data(), (int) s.size()); // also overruns the ring
    }
    CHECK(RtSafety::violations() == 0);
}

namespace
{
std::vector<float> harm(double hz, double sr, int n, float a1, float a2, float a3)
{
    std::vector<float> v((size_t) n);
    for (int i = 0; i < n; ++i)
    {
        const double ph = 2.0 * kPi * hz * i / sr;
        v[(size_t) i] = a1 * (float) std::sin(ph) + a2 * (float) std::sin(2 * ph) + a3 * (float) std::sin(3 * ph);
    }
    return v;
}
} // namespace

TEST_CASE("tuner: overflow discards stale audio and resyncs")
{
    TunerTap t; t.prepare(48000.0);
    feed(t, sine(110.0, 48000.0, 20000)); // > ring size (8192) with no polling
    CHECK_FALSE(t.poll().has_value());     // overflow detected: resync, nothing analysed
    feed(t, sine(196.0, 48000.0, 3000));   // fresh window
    auto r = t.poll();
    REQUIRE(r.has_value());
    CHECK(r->note == "G");
    CHECK(r->octave == 3);
}

TEST_CASE("tuner: reset discards buffered audio")
{
    TunerTap t; t.prepare(48000.0);
    feed(t, sine(110.0, 48000.0, 5000));
    t.reset();
    CHECK_FALSE(t.poll().has_value());
    feed(t, sine(196.0, 48000.0, 3000));
    auto r = t.poll();
    REQUIRE(r.has_value());
    CHECK(r->note == "G");
}

TEST_CASE("tuner: strong 2nd harmonic keeps the fundamental octave")
{
    for (double f : { 82.41, 196.0 })
    {
        TunerTap t; t.prepare(48000.0);
        feed(t, harm(f, 48000.0, 6000, 0.3f, 1.2f, 0.1f));
        auto r = t.poll();
        REQUIRE(r.has_value());
        CHECK_THAT(r->hz, Catch::Matchers::WithinRel((float) f, 0.02f));
    }
}

TEST_CASE("tuner: repeated poll without new audio repeats then expires")
{
    TunerTap t; t.prepare(48000.0);
    feed(t, sine(110.0, 48000.0, 3000));
    REQUIRE(t.poll().has_value());
    for (int i = 0; i < TunerTap::kMaxStalePolls; ++i) CHECK(t.poll().has_value());
    CHECK_FALSE(t.poll().has_value());
}
