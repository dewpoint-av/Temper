#pragma once

/*
  Temper — filter maths shared by the DSP and the UI (so the drawn curve is the processed one).

  One "filter node" (an EQ band or a detector pre-filter) is a type, frequency, gain, Q and,
  for the cut types, a slope: up to four RBJ biquads in series (Bristow-Johnson's Audio EQ
  Cookbook). JUCE-free.
*/

#include <algorithm>
#include <array>
#include <cmath>

namespace temper
{
    enum class FilterType : int { Bell = 0, LowShelf, HighShelf, LowCut, HighCut, Notch, BandPass, Tilt, Count };

    inline const char* filterTypeName (int t)
    {
        static const char* names[] = { "Bell", "Low Shelf", "High Shelf", "Low Cut", "High Cut", "Notch", "Band Pass", "Tilt" };
        return (t >= 0 && t < static_cast<int> (FilterType::Count)) ? names[t] : "Bell";
    }
    // Whether the gain applies to this type (cuts, notch and band pass have none).
    inline bool filterHasGain (int t)
    {
        return t == static_cast<int> (FilterType::Bell) || t == static_cast<int> (FilterType::LowShelf)
            || t == static_cast<int> (FilterType::HighShelf) || t == static_cast<int> (FilterType::Tilt);
    }
    inline bool filterHasSlope (int t)
    {
        return t == static_cast<int> (FilterType::LowCut) || t == static_cast<int> (FilterType::HighCut);
    }

    // Where an EQ band acts in a stereo signal.
    enum class Channel : int { Stereo = 0, Left, Right, Mid, Side };

    struct FilterNode
    {
        bool  on = false;
        int   type = 0;          // FilterType
        float freq = 1000.0f;    // Hz
        float gainDb = 0.0f;
        float q = 1.0f;
        int   slope = 0;         // cuts: 0 = 12, 1 = 24, 2 = 48 dB/oct
        int   channel = 0;       // EQ only: Channel

        bool operator== (const FilterNode& o) const
        {
            return on == o.on && type == o.type && freq == o.freq && gainDb == o.gainDb
                && q == o.q && slope == o.slope && channel == o.channel;
        }
        bool operator!= (const FilterNode& o) const { return ! (*this == o); }
    };

    struct BiquadCoefs { double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0; };

    // The biquads of one node at sample rate fs. Returns how many (0..4).
    inline int makeNodeCoefs (const FilterNode& nd, double fs, std::array<BiquadCoefs, 4>& out)
    {
        const double pi = 3.14159265358979323846;
        const double f  = std::clamp (static_cast<double> (nd.freq), 10.0, fs * 0.49);
        const double q  = std::clamp (static_cast<double> (nd.q), 0.05, 40.0);
        const double w0 = 2.0 * pi * f / fs;
        const double cw = std::cos (w0), sw = std::sin (w0);

        const auto norm = [] (double b0, double b1, double b2, double a0, double a1, double a2)
        {
            BiquadCoefs c; c.b0 = b0 / a0; c.b1 = b1 / a0; c.b2 = b2 / a0; c.a1 = a1 / a0; c.a2 = a2 / a0; return c;
        };
        const auto peak = [&] (double gainDb, double qq)
        {
            const double A = std::pow (10.0, gainDb / 40.0), alpha = sw / (2.0 * qq);
            return norm (1 + alpha * A, -2 * cw, 1 - alpha * A, 1 + alpha / A, -2 * cw, 1 - alpha / A);
        };
        const auto lowShelf = [&] (double gainDb, double qq)
        {
            const double A = std::pow (10.0, gainDb / 40.0), alpha = sw / (2.0 * qq), s = 2.0 * std::sqrt (A) * alpha;
            return norm (A * ((A + 1) - (A - 1) * cw + s), 2 * A * ((A - 1) - (A + 1) * cw), A * ((A + 1) - (A - 1) * cw - s),
                         (A + 1) + (A - 1) * cw + s, -2 * ((A - 1) + (A + 1) * cw), (A + 1) + (A - 1) * cw - s);
        };
        const auto highShelf = [&] (double gainDb, double qq)
        {
            const double A = std::pow (10.0, gainDb / 40.0), alpha = sw / (2.0 * qq), s = 2.0 * std::sqrt (A) * alpha;
            return norm (A * ((A + 1) + (A - 1) * cw + s), -2 * A * ((A - 1) + (A + 1) * cw), A * ((A + 1) + (A - 1) * cw - s),
                         (A + 1) - (A - 1) * cw + s, 2 * ((A - 1) - (A + 1) * cw), (A + 1) - (A - 1) * cw - s);
        };
        const auto lp = [&] (double qq) { const double alpha = sw / (2.0 * qq); return norm ((1 - cw) / 2, 1 - cw, (1 - cw) / 2, 1 + alpha, -2 * cw, 1 - alpha); };
        const auto hp = [&] (double qq) { const double alpha = sw / (2.0 * qq); return norm ((1 + cw) / 2, -(1 + cw), (1 + cw) / 2, 1 + alpha, -2 * cw, 1 - alpha); };

        switch (static_cast<FilterType> (nd.type))
        {
            case FilterType::Bell:      out[0] = peak (nd.gainDb, q); return 1;
            case FilterType::LowShelf:  out[0] = lowShelf (nd.gainDb, q); return 1;
            case FilterType::HighShelf: out[0] = highShelf (nd.gainDb, q); return 1;
            case FilterType::Tilt:      out[0] = lowShelf (-0.5 * nd.gainDb, q); out[1] = highShelf (0.5 * nd.gainDb, q); return 2;
            case FilterType::Notch:
            {
                const double alpha = sw / (2.0 * q);
                out[0] = norm (1, -2 * cw, 1, 1 + alpha, -2 * cw, 1 - alpha); return 1;
            }
            case FilterType::BandPass:
            {
                const double alpha = sw / (2.0 * q);
                out[0] = norm (alpha, 0, -alpha, 1 + alpha, -2 * cw, 1 - alpha); return 1;
            }
            case FilterType::LowCut:
            case FilterType::HighCut:
            {
                // Butterworth sections; Q scales the last (most resonant) one, so 0.707 is flat.
                static const double q24[2] = { 0.54119610, 1.30656296 };
                static const double q48[4] = { 0.50979558, 0.60134489, 0.89997622, 2.56291545 };
                const bool hi = static_cast<FilterType> (nd.type) == FilterType::LowCut;   // a low cut is a highpass
                const double res = q / 0.70710678;
                if (nd.slope <= 0) { out[0] = hi ? hp (q) : lp (q); return 1; }
                const int count = (nd.slope == 1) ? 2 : 4;
                const double* qs = (nd.slope == 1) ? q24 : q48;
                for (int i = 0; i < count; ++i)
                {
                    const double qq = (i == count - 1) ? qs[i] * res : qs[i];
                    out[static_cast<size_t> (i)] = hi ? hp (qq) : lp (qq);
                }
                return count;
            }
            default: break;
        }
        out[0] = BiquadCoefs(); return 1;
    }

    // |H| in dB of a node at frequency `freq`.
    inline float nodeResponseDb (const FilterNode& nd, double fs, double freq)
    {
        if (! nd.on) return 0.0f;
        std::array<BiquadCoefs, 4> c;
        const int count = makeNodeCoefs (nd, fs, c);
        const double w = 2.0 * 3.14159265358979323846 * std::clamp (freq, 1.0, fs * 0.5) / fs;
        const double c1 = std::cos (w), s1 = std::sin (w), c2 = std::cos (2 * w), s2 = std::sin (2 * w);
        double mag2 = 1.0;
        for (int i = 0; i < count; ++i)
        {
            const auto& k = c[static_cast<size_t> (i)];
            const double nr = k.b0 + k.b1 * c1 + k.b2 * c2, ni = -(k.b1 * s1 + k.b2 * s2);
            const double dr = 1.0 + k.a1 * c1 + k.a2 * c2, di = -(k.a1 * s1 + k.a2 * s2);
            mag2 *= (nr * nr + ni * ni) / std::max (dr * dr + di * di, 1e-30);
        }
        return static_cast<float> (10.0 * std::log10 (std::max (mag2, 1e-20)));
    }

    // Transposed direct form II, double state (low-frequency bells stay clean).
    class Biquad2
    {
    public:
        void set (const BiquadCoefs& c) { k = c; }
        void reset() { z1 = z2 = 0.0; }
        float process (float xf)
        {
            const double x = xf;
            const double y = k.b0 * x + z1;
            z1 = k.b1 * x - k.a1 * y + z2;
            z2 = k.b2 * x - k.a2 * y;
            return static_cast<float> (y);
        }
    private:
        BiquadCoefs k;
        double z1 = 0.0, z2 = 0.0;
    };
} // namespace temper
