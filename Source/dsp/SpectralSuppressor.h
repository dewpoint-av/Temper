#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <vector>

// Temper — dynamic resonance suppressor (Soothe2 / DSEQ3 style).
//
// Two stages run in series:
//   1. Dynamic suppression: a bank of narrow peaking filters whose gain is
//      driven by the level of a matching bandpass filter (fast attack, slow
//      release) so resonances are tamed only when they ring.
//   2. Manual EQ: a set of user-placed peaking "nodes" (frequency + gain in dB)
//      edited graphically on the spectrum, like DSEQ3.

namespace temper
{
    static constexpr int kNumBands   = 16;   // dynamic suppression bands
    static constexpr int kNumEqBands = 8;    // manual EQ nodes

    struct Params
    {
        float amount      = 0.5f;    // 0..1  overall cut depth
        float sensitivity = 0.5f;    // 0..1  higher = catches quieter resonances
        float slope       = 0.5f;    // 0..1  suppression aggressiveness (knee)
        float q           = 0.5f;    // 0..1  bandwidth (0 = narrow, 1 = wide)
        float attackMs    = 10.0f;   // ms
        float releaseMs   = 120.0f;  // ms
        float mix         = 1.0f;    // 0..1
        float output      = 1.0f;    // 0..2

        float eqFreq[kNumEqBands] = { 120.0f, 250.0f, 500.0f, 1000.0f, 2000.0f, 4000.0f, 8000.0f, 12000.0f };
        float eqGain[kNumEqBands] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };  // dB, 0 = inactive

        bool operator== (const Params& o) const
        {
            if (amount      != o.amount
                || sensitivity != o.sensitivity
                || slope       != o.slope
                || q           != o.q
                || attackMs    != o.attackMs
                || releaseMs   != o.releaseMs
                || mix         != o.mix
                || output      != o.output)
                return false;

            for (int i = 0; i < kNumEqBands; ++i)
                if (eqFreq[i] != o.eqFreq[i] || eqGain[i] != o.eqGain[i])
                    return false;

            return true;
        }
    };

    class SpectralSuppressor
    {
    public:
        void prepare (double sampleRate, int maxBlockSize);
        void reset();
        void setParams (const Params& p);
        void process (const float* inL, const float* inR,
                      float* outL, float* outR, int numSamples);

        // Per-band dynamic gain reduction in dB (negative while suppressing), for the UI.
        float getReductionDb (int band) const;

        // Recent mono input for the spectrum display.
        const std::vector<float>& vizInput() const { return vizInputBuf; }
        int  vizWritePos() const                    { return vizWrite.load(); }

    private:
        struct Biquad
        {
            float b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
            float z1 = 0, z2 = 0;

            void reset() { z1 = z2 = 0.0f; }

            float process (float x)
            {
                const float y = b0 * x + z1;
                z1 = b1 * x - a1 * y + z2;
                z2 = b2 * x - a2 * y;
                return y;
            }

            void setPeaking (float fs, float freq, float q, float gainDb)
            {
                const float A     = std::pow (10.0f, gainDb / 40.0f);
                const float w0    = 2.0f * 3.14159265358979323846f * freq / fs;
                const float alpha = std::sin (w0) / (2.0f * q);
                const float cw    = std::cos (w0);
                const float a0    = 1.0f + alpha / A;
                b0 = (1.0f + alpha * A) / a0;
                b1 = (-2.0f * cw) / a0;
                b2 = (1.0f - alpha * A) / a0;
                a1 = (-2.0f * cw) / a0;
                a2 = (1.0f - alpha / A) / a0;
            }

            void setBandpass (float fs, float freq, float q)
            {
                const float w0    = 2.0f * 3.14159265358979323846f * freq / fs;
                const float alpha = std::sin (w0) / (2.0f * q);
                const float cw    = std::cos (w0);
                const float a0    = 1.0f + alpha;
                b0 = alpha / a0;
                b1 = 0.0f;
                b2 = -alpha / a0;
                a1 = (-2.0f * cw) / a0;
                a2 = (1.0f - alpha) / a0;
            }
        };

        struct Band
        {
            Biquad detect;   // bandpass — measures the resonance
            Biquad cut;      // peaking  — applies the reduction
            float freq      = 0.0f;
            float env       = 0.0f;
            float gainDb    = 0.0f;
            float appliedDb = 999.0f;
        };

        struct EqBand
        {
            Biquad filter;
            float freq   = 0.0f;
            float gainDb = 0.0f;
        };

        void update (double sr);

        double sampleRate = 48000.0;
        Params p;
        float attCoef      = 0.0f;
        float relCoef      = 0.0f;
        float thresholdLin = 0.01f;
        float maxCutDb     = 24.0f;
        float slopeFactor  = 1.0f;
        float qCut         = 2.0f;
        float qDetect      = 1.5f;

        std::array<Band,   kNumBands>   bands[2];
        std::array<EqBand, kNumEqBands> eq[2];

        static constexpr int kVizSize = 4096;
        std::vector<float> vizInputBuf;
        std::atomic<int> vizWrite { 0 };
    };

} // namespace temper
