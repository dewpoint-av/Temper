#pragma once

/*
  Temper — a small in-place radix-2 complex FFT (split real / imaginary arrays).
  JUCE-free so the spectral engine can be tested on its own. Sizes are powers of two.
  The inverse is unscaled (divide by N yourself).
*/

#include <cmath>
#include <utility>
#include <vector>

namespace temper
{
    class Fft
    {
    public:
        void init (int size)
        {
            if (size == n) return;
            n = size;
            cosT.resize (static_cast<size_t> (n / 2));
            sinT.resize (static_cast<size_t> (n / 2));
            for (int k = 0; k < n / 2; ++k)
            {
                const double a = 2.0 * 3.14159265358979323846 * k / n;
                cosT[static_cast<size_t> (k)] = static_cast<float> (std::cos (a));
                sinT[static_cast<size_t> (k)] = static_cast<float> (std::sin (a));
            }
            rev.resize (static_cast<size_t> (n));
            int bits = 0;
            while ((1 << bits) < n) ++bits;
            for (int i = 0; i < n; ++i)
            {
                int r = 0;
                for (int b = 0; b < bits; ++b)
                    if (i & (1 << b)) r |= 1 << (bits - 1 - b);
                rev[static_cast<size_t> (i)] = r;
            }
        }

        int size() const { return n; }

        // Forward: X[k] = sum x[t] e^{-2 pi i k t / N}. Inverse: the + sign, unscaled.
        void transform (float* re, float* im, bool inverse) const
        {
            for (int i = 0; i < n; ++i)
            {
                const int j = rev[static_cast<size_t> (i)];
                if (j > i) { std::swap (re[i], re[j]); std::swap (im[i], im[j]); }
            }
            const float sgn = inverse ? 1.0f : -1.0f;
            for (int len = 2; len <= n; len <<= 1)
            {
                const int half = len >> 1;
                const int step = n / len;
                for (int i = 0; i < n; i += len)
                {
                    for (int k = 0; k < half; ++k)
                    {
                        const float tr = cosT[static_cast<size_t> (k * step)];
                        const float ti = sgn * sinT[static_cast<size_t> (k * step)];
                        const int a = i + k, b = a + half;
                        const float xr = re[b] * tr - im[b] * ti;
                        const float xi = re[b] * ti + im[b] * tr;
                        re[b] = re[a] - xr; im[b] = im[a] - xi;
                        re[a] += xr;        im[a] += xi;
                    }
                }
            }
        }

    private:
        int n = 0;
        std::vector<float> cosT, sinT;
        std::vector<int> rev;
    };
} // namespace temper
