#pragma once

/*
  Temper — band splitting for the OTT and gate (JUCE-free).

  Time domain: Linkwitz-Riley 4th-order crossovers (two Butterworth sections each). Three
  bands: split at f1, then the upper part at f2; the low band goes through the matching
  allpass (LP2 + HP2 of itself), so the three bands sum to an allpass: flat magnitude, no
  bump or notch at the crossovers.

  Frequency domain: complementary raised-cosine masks, a third of an octave wide around each
  crossover, that sum to exactly 1 in every bin (linear phase, no crossover colouring).
*/

#include "Filters.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace temper
{
    static constexpr int kDynBands = 3;   // OTT and gate: low / mid / high

    class Lr4Split
    {
    public:
        void set (double fs, float f1, float f2, int bands)
        {
            numBands = std::clamp (bands, 1, kDynBands);
            f1 = std::clamp (f1, 20.0f, static_cast<float> (fs * 0.45));
            f2 = std::clamp (std::max (f2, f1 * 1.26f), 20.0f, static_cast<float> (fs * 0.45));
            const auto bw = [fs] (int type, float f)
            {
                FilterNode n; n.on = true; n.type = type; n.freq = f; n.q = 0.70710678f; n.slope = 0;
                std::array<BiquadCoefs, 4> c; makeNodeCoefs (n, fs, c); return c[0];
            };
            const int LC = static_cast<int> (FilterType::LowCut), HC = static_cast<int> (FilterType::HighCut);
            const auto lp1 = bw (HC, f1), hp1 = bw (LC, f1), lp2 = bw (HC, f2), hp2 = bw (LC, f2);
            for (auto* b : { &a[0], &a[1] }) b->set (lp1);
            for (auto* b : { &a[2], &a[3] }) b->set (hp1);
            for (auto* b : { &a[4], &a[5], &a[8], &a[9] }) b->set (lp2);
            for (auto* b : { &a[6], &a[7], &a[10], &a[11] }) b->set (hp2);
        }
        void reset() { for (auto& b : a) b.reset(); }
        int bands() const { return numBands; }

        void process (float x, float* out)
        {
            if (numBands == 1) { out[0] = x; return; }
            const float lo = a[1].process (a[0].process (x));
            const float hi = a[3].process (a[2].process (x));
            if (numBands == 2) { out[0] = lo; out[1] = hi; return; }
            out[1] = a[5].process (a[4].process (hi));
            out[2] = a[7].process (a[6].process (hi));
            out[0] = a[9].process (a[8].process (lo)) + a[11].process (a[10].process (lo));   // allpass at f2
        }

    private:
        std::array<Biquad2, 12> a;
        int numBands = 1;
    };

    // Weight of band b (0..bands-1) at frequency f; the weights sum to 1.
    inline float bandMask (int bands, float f1, float f2, float f, int b)
    {
        bands = std::clamp (bands, 1, kDynBands);
        if (bands == 1) return 1.0f;
        const auto rise = [f] (float fc)   // 0 below fc / 2^(1/6), 1 above fc * 2^(1/6), raised cosine between
        {
            const float t = std::clamp ((std::log2 (std::max (f, 1.0f) / fc) + 1.0f / 6.0f) * 3.0f, 0.0f, 1.0f);
            return 0.5f - 0.5f * std::cos (3.14159265f * t);
        };
        f2 = std::max (f2, f1 * 1.26f);   // keep the two transitions apart
        const float s1 = rise (f1);
        if (bands == 2) return b == 0 ? 1.0f - s1 : s1;
        const float s2 = rise (f2);
        return b == 0 ? 1.0f - s1 : (b == 1 ? std::max (s1 - s2, 0.0f) : s2);
    }

    // A fixed delay (lookahead), up to its allocated length.
    class DelayLine
    {
    public:
        void prepare (int maxLen) { buf.assign (static_cast<size_t> (std::max (maxLen, 1)) + 1, 0.0f); pos = 0; }
        void reset() { std::fill (buf.begin(), buf.end(), 0.0f); pos = 0; }
        float process (float x, int delay)
        {
            const int n = static_cast<int> (buf.size());
            buf[static_cast<size_t> (pos)] = x;
            int rd = pos - std::clamp (delay, 0, n - 1);
            if (rd < 0) rd += n;
            const float y = buf[static_cast<size_t> (rd)];
            pos = (pos + 1) % n;
            return y;
        }
    private:
        std::vector<float> buf;
        int pos = 0;
    };

    // Two spectral resolutions share the work: a long FFT below fc, a short one above, crossed
    // over with a raised cosine two octaves wide (wide enough that both grids sample it finely
    // and their sum stays flat). lowShare + highShare = 1 at every frequency.
    inline float lowResShare (float f, float fc)
    {
        const float t = std::clamp (0.5f * std::log2 (std::max (f, 1.0f) / fc) + 0.5f, 0.0f, 1.0f);
        return 0.5f + 0.5f * std::cos (3.14159265f * t);
    }

    // Level detector for a compressor band: a two-pole RMS (or a smoothed peak) whose time
    // constant suits the band's lowest frequency, so a sub tone reads as a steady level
    // instead of a ripple at twice its frequency (ripple becomes harmonic distortion through
    // the gain). Returns the level in dB (a full-scale sine reads 0 dB in RMS).
    class LevelDetector
    {
    public:
        void set (double fs, float tauSec, bool peakMode)
        {
            peak = peakMode;
            c = 1.0f - std::exp (-1.0f / (std::max (tauSec, 0.0001f) * static_cast<float> (fs)));
            // Peak: the hold decays over the same time, the two smoothing poles are 4x quicker.
            decay = std::exp (-1.0f / (std::max (tauSec, 0.0001f) * static_cast<float> (fs)));
            cp = 1.0f - std::exp (-4.0f / (std::max (tauSec, 0.0001f) * static_cast<float> (fs)));
        }
        void reset() { s1 = s2 = pk = 0.0f; }
        float processDb (float x)
        {
            if (peak)
            {
                pk = std::max (std::fabs (x), pk * decay);
                s1 += cp * (pk - s1);
                s2 += cp * (s1 - s2);
                return 10.0f * std::log10 (s2 * s2 * 0.5f + 1.0e-20f);   // peak -> the RMS a sine of that peak would have
            }
            s1 += c * (x * x - s1);
            s2 += c * (s1 - s2);
            return 10.0f * std::log10 (s2 + 1.0e-20f);
        }
        float lastPower() const { return peak ? s2 * s2 * 0.5f : s2; }
    private:
        bool peak = false;
        float c = 0.0f, cp = 0.0f, decay = 0.0f, s1 = 0.0f, s2 = 0.0f, pk = 0.0f;
    };

    // A hard clipper with first-order antiderivative anti-aliasing (Parker et al. 2016) applied
    // to the clipping residual only, so the linear region is untouched (no half-sample
    // low-pass) while the clipped part's aliasing drops by about 17 dB. The anti-aliased result
    // overshoots the ceiling a little where the clip starts (as a band-limited clip must); a
    // final clamp holds the ceiling at the cost of a few dB of that gain.
    class AdaaClipper
    {
    public:
        void reset() { x1 = 0.0f; }
        float process (float x, float c)
        {
            const auto R = [c] (float v) { const float e = std::fabs (v) - c; return e > 0.0f ? -0.5f * e * e : 0.0f; };   // antiderivative of clip(v) - v
            const float d = x - x1;
            float r;
            if (std::fabs (d) > 1.0e-5f) r = (R (x) - R (x1)) / d;
            else { const float m = 0.5f * (x + x1); r = std::clamp (m, -c, c) - m; }
            x1 = x;
            return std::clamp (x + r, -c, c);
        }
    private:
        float x1 = 0.0f;
    };

    // A running mean over the last `len` samples (lookahead gain smoothing).
    class RunningMean
    {
    public:
        void prepare (int maxLen) { buf.assign (static_cast<size_t> (std::max (maxLen, 1)), 0.0f); pos = 0; sum = 0.0; cur = 0; }
        void reset() { std::fill (buf.begin(), buf.end(), 0.0f); pos = 0; sum = 0.0; }
        float process (float x, int len)
        {
            len = std::clamp (len, 1, static_cast<int> (buf.size()));
            if (len != cur) { cur = len; sum = 0.0; std::fill (buf.begin(), buf.end(), x); sum = static_cast<double> (x) * len; }
            if (len == 1) return x;
            sum += x - buf[static_cast<size_t> (pos)];
            buf[static_cast<size_t> (pos)] = x;
            pos = (pos + 1) % len;
            return static_cast<float> (sum / len);
        }
    private:
        std::vector<float> buf;
        int pos = 0, cur = 0;
        double sum = 0.0;
    };
} // namespace temper
