#include "TunerTap.h"
#include <algorithm>
#include <cmath>

namespace amp
{
void TunerTap::prepare(double sampleRate)
{
    decim = std::max(1, (int) std::lround(sampleRate / 48000.0));
    effectiveRate = sampleRate / decim;
    const int rawWindow = kWindow * decim;
    const int ringSize = std::max(8192, 4 * rawWindow);
    ring.assign((size_t) ringSize, 0.0f);
    fifo.setTotalSize(ringSize);
    fifo.reset();
    history.assign((size_t) rawWindow, 0.0f);
    window.assign((size_t) kWindow, 0.0f);
    diff.assign((size_t) kWindow, 0.0f);
    historyPos = 0;
    consumed = 0;
}

void TunerTap::push(const float* samples, int numSamples)
{
    if (ring.empty() || samples == nullptr || numSamples <= 0)
        return;
    int s1, n1, s2, n2;
    fifo.prepareToWrite(numSamples, s1, n1, s2, n2); // writes as many as fit; the rest (newest) are dropped
    if (n1 > 0) std::copy(samples, samples + n1, ring.begin() + s1);
    if (n2 > 0) std::copy(samples + n1, samples + n1 + n2, ring.begin() + s2);
    fifo.finishedWrite(n1 + n2);
}

std::optional<TunerReading> TunerTap::poll()
{
    if (ring.empty())
        return std::nullopt;

    const int rawWindow = (int) history.size();
    const int ready = fifo.getNumReady();
    if (ready > 0)
    {
        int s1, n1, s2, n2;
        fifo.prepareToRead(ready, s1, n1, s2, n2);
        auto take = [&](int start, int n) {
            for (int i = 0; i < n; ++i)
            {
                history[(size_t) historyPos] = ring[(size_t) (start + i)];
                historyPos = (historyPos + 1) % rawWindow;
            }
        };
        take(s1, n1);
        take(s2, n2);
        fifo.finishedRead(n1 + n2);
        consumed += n1 + n2;
    }
    if (consumed < rawWindow)
        return std::nullopt;

    // Linearise oldest -> newest (historyPos is the oldest sample), decimating by averaging.
    for (int i = 0; i < kWindow; ++i)
    {
        float sum = 0.0f;
        for (int k = 0; k < decim; ++k)
            sum += history[(size_t) ((historyPos + i * decim + k) % rawWindow)];
        window[(size_t) i] = sum / (float) decim;
    }

    double energy = 0.0;
    for (float v : window) energy += (double) v * v;
    const double rms = std::sqrt(energy / kWindow);
    if (rms < std::pow(10.0, kSilenceDb / 20.0))
        return std::nullopt;

    const int tauMin = std::max(2, (int) std::floor(effectiveRate / kMaxHz));
    const int tauMax = std::min(kWindow / 2, (int) std::ceil(effectiveRate / kMinHz));
    const int w = kWindow - tauMax - 1; // integration length, constant across lags
    if (w <= 0 || tauMax <= tauMin + 2)
        return std::nullopt;

    // Difference function + cumulative mean normalised difference.
    const float* x = window.data();
    diff[0] = 1.0f;
    double running = 0.0;
    for (int tau = 1; tau <= tauMax; ++tau)
    {
        double d = 0.0;
        for (int j = 0; j < w; ++j)
        {
            const double e = (double) x[j] - (double) x[j + tau];
            d += e * e;
        }
        running += d;
        diff[(size_t) tau] = running > 0.0 ? (float) (d * tau / running) : 1.0f;
    }

    // Absolute threshold: first lag below it, then descend to the local minimum.
    int tau = -1;
    for (int t = tauMin; t < tauMax; ++t)
    {
        if (diff[(size_t) t] < kThreshold)
        {
            while (t + 1 < tauMax && diff[(size_t) t + 1] < diff[(size_t) t]) ++t;
            tau = t;
            break;
        }
    }
    if (tau < 0)
        return std::nullopt;

    // Parabolic interpolation around the minimum.
    double refined = tau;
    if (tau > 1 && tau + 1 <= tauMax)
    {
        const double a = diff[(size_t) tau - 1], b = diff[(size_t) tau], c = diff[(size_t) tau + 1];
        const double denom = a - 2.0 * b + c;
        if (std::abs(denom) > 1e-12)
            refined = tau + 0.5 * (a - c) / denom;
    }
    const double hz = effectiveRate / refined;
    if (! std::isfinite(hz) || hz < kMinHz * 0.9 || hz > kMaxHz * 1.1)
        return std::nullopt;

    const double midi = 69.0 + 12.0 * std::log2(hz / 440.0);
    int nearest = (int) std::lround(midi);
    double cents = (midi - nearest) * 100.0;
    if (cents >= 50.0) { ++nearest; cents -= 100.0; }
    if (cents < -50.0) { --nearest; cents += 100.0; }

    static const char* names[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    TunerReading r;
    r.hz = (float) hz;
    r.note = names[((nearest % 12) + 12) % 12];
    r.octave = (int) std::floor(nearest / 12.0) - 1;
    r.cents = (float) cents;
    return r;
}
} // namespace amp
