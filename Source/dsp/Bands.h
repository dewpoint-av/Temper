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
} // namespace temper
