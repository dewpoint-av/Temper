#pragma once

/*
  Temper — a stereo short-time Fourier transform with an optional sidechain, shared by the
  spectral OTT and the spectral gate (JUCE-free).

  Hann windows at 8x overlap; both channels go through one complex FFT. On every hop it
  hands the caller the two spectra and their detector power per bin (a full-scale sine reads
  1 in its bin), plus the sidechain's if it is used; the caller fills a real gain per bin and
  channel (zero phase), and the result is overlap-added back. Latency = the FFT size.
*/

#include "Fft.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace temper
{
    class StereoStft
    {
    public:
        void prepare (int maxSize)
        {
            maxN = maxSize;
            const size_t N = static_cast<size_t> (maxN), B = N / 2 + 1;
            for (int c = 0; c < 2; ++c)
            {
                in[c].assign (N, 0.0f); sc[c].assign (N, 0.0f); out[c].assign (N, 0.0f);
                xr[c].assign (B, 0.0f); xi[c].assign (B, 0.0f); pow[c].assign (B, 0.0f); scPow[c].assign (B, 0.0f);
                gain[c].assign (B, 1.0f);
            }
            re.assign (N, 0.0f); im.assign (N, 0.0f); window.assign (N, 0.0f);
            n = 0;
        }

        void configure (int size)
        {
            n = std::min (size, maxN);
            hop = n / 8; half = n / 2;
            fft.init (n);
            for (int i = 0; i < n; ++i)
                window[static_cast<size_t> (i)] = 0.5f - 0.5f * std::cos (2.0f * 3.14159265358979323846f * static_cast<float> (i) / static_cast<float> (n));
            double s = 0.0;
            for (int m = 0; m < 8; ++m) { const double w = window[static_cast<size_t> ((hop / 2 + m * hop) % n)]; s += w * w; }
            olaScale = static_cast<float> (1.0 / (n * s));
            reset();
        }

        void reset()
        {
            for (int c = 0; c < 2; ++c)
            {
                std::fill (in[c].begin(), in[c].end(), 0.0f); std::fill (sc[c].begin(), sc[c].end(), 0.0f);
                std::fill (out[c].begin(), out[c].end(), 0.0f); std::fill (gain[c].begin(), gain[c].end(), 1.0f);
            }
            wpos = rpos = hopCount = 0;
        }

        int size() const { return n; }
        int bins() const { return half + 1; }
        int hopSize() const { return hop; }

        // One stereo sample; useSc: also analyse the sidechain. onFrame (const StereoStft&, gains)
        // runs on each hop and fills gain[c][k].
        template <typename F>
        void process (float& l, float& r, float scl, float scr, bool useSc, F&& onFrame)
        {
            const size_t mask = static_cast<size_t> (n - 1);
            in[0][static_cast<size_t> (wpos)] = l; in[1][static_cast<size_t> (wpos)] = r;
            sc[0][static_cast<size_t> (wpos)] = scl; sc[1][static_cast<size_t> (wpos)] = scr;
            wpos = (wpos + 1) & (n - 1);
            l = out[0][static_cast<size_t> (rpos)]; r = out[1][static_cast<size_t> (rpos)];
            out[0][static_cast<size_t> (rpos)] = 0.0f; out[1][static_cast<size_t> (rpos)] = 0.0f;
            rpos = (rpos + 1) & (n - 1);
            if (++hopCount < hop) return;
            hopCount = 0;

            const float norm2 = (4.0f / static_cast<float> (n)) * (4.0f / static_cast<float> (n));
            if (useSc)
            {
                analyse (sc, mask);
                for (int c = 0; c < 2; ++c)
                    for (int k = 0; k <= half; ++k)
                        scPow[c][static_cast<size_t> (k)] = (xr[c][static_cast<size_t> (k)] * xr[c][static_cast<size_t> (k)] + xi[c][static_cast<size_t> (k)] * xi[c][static_cast<size_t> (k)]) * norm2;
            }
            analyse (in, mask);
            for (int c = 0; c < 2; ++c)
                for (int k = 0; k <= half; ++k)
                    pow[c][static_cast<size_t> (k)] = (xr[c][static_cast<size_t> (k)] * xr[c][static_cast<size_t> (k)] + xi[c][static_cast<size_t> (k)] * xi[c][static_cast<size_t> (k)]) * norm2;

            onFrame (*this, gain);

            for (int k = 0; k <= half; ++k)
            {
                const size_t a = static_cast<size_t> (k);
                const float lr = xr[0][a] * gain[0][a], li = xi[0][a] * gain[0][a];
                const float rr = xr[1][a] * gain[1][a], ri = xi[1][a] * gain[1][a];
                re[a] = lr - ri; im[a] = li + rr;
                if (k > 0 && k < half) { const size_t b = static_cast<size_t> (n - k); re[b] = lr + ri; im[b] = rr - li; }
            }
            fft.transform (re.data(), im.data(), true);
            for (int i = 0; i < n; ++i)
            {
                const size_t o = (static_cast<size_t> (rpos) + static_cast<size_t> (i)) & mask;
                const float w = window[static_cast<size_t> (i)] * olaScale;
                out[0][o] += re[static_cast<size_t> (i)] * w;
                out[1][o] += im[static_cast<size_t> (i)] * w;
            }
        }

        // Read by onFrame.
        std::array<std::vector<float>, 2> pow, scPow;   // detector power per bin
        std::array<std::vector<float>, 2> gain;         // written by onFrame (linear)

    private:
        void analyse (const std::array<std::vector<float>, 2>& src, size_t mask)
        {
            for (int i = 0; i < n; ++i)
            {
                const size_t idx = (static_cast<size_t> (wpos) + static_cast<size_t> (i)) & mask;
                const float w = window[static_cast<size_t> (i)];
                re[static_cast<size_t> (i)] = src[0][idx] * w;
                im[static_cast<size_t> (i)] = src[1][idx] * w;
            }
            fft.transform (re.data(), im.data(), false);
            for (int k = 0; k <= half; ++k)
            {
                const size_t a = static_cast<size_t> (k), b = static_cast<size_t> ((n - k) & (n - 1));
                xr[0][a] = 0.5f * (re[a] + re[b]); xi[0][a] = 0.5f * (im[a] - im[b]);
                xr[1][a] = 0.5f * (im[a] + im[b]); xi[1][a] = -0.5f * (re[a] - re[b]);
            }
        }

        int n = 0, maxN = 0, hop = 0, half = 0, wpos = 0, rpos = 0, hopCount = 0;
        float olaScale = 1.0f;
        Fft fft;
        std::vector<float> re, im, window;
        std::array<std::vector<float>, 2> in, sc, out, xr, xi;
    };
} // namespace temper
