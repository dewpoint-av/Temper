#pragma once

/*
  Temper — modular spectral processor.

  Three frequency-domain modules run in a user-reorderable serial chain,
  followed by a static 8-node manual EQ (DSEQ3-style):

    1. Resonance suppressor  — dynamic suppression (Soothe2-style): per-band
       detection drives a matching peaking cut so resonances are tamed only
       when they ring.
    2. Spectral OTT          — multiband upward + downward compression: the
       exaggerated "over the top" sound-design compressor.
    3. Spectral gate         — per-band downward attenuation below a threshold,
       to strip low-level rumble / noise from selected regions.

  Each module has 1..5 bands with per-band settings and shared adjustable
  crossovers. The OTT and gate are off by default. The modules split via
  cascaded 2nd-order Butterworth lowpasses, so the bands sum back to the input
  exactly (zero-latency, perfect reconstruction).
*/

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <vector>

namespace temper
{
    static constexpr int kMaxBands      = 5;
    static constexpr int kMaxCrossovers = kMaxBands - 1;   // 4
    static constexpr int kNumModules    = 3;
    static constexpr int kNumEqBands    = 8;               // manual EQ nodes (unchanged)

    enum class Module : int { Suppressor = 0, Ott = 1, Gate = 2 };

    // -----------------------------------------------------------------------
    // A cascaded 2nd-order Butterworth lowpass bank: band i = low[i] - low[i-1],
    // top band = x - low[last]; the bands sum to x exactly.
    // -----------------------------------------------------------------------
    class Biquad
    {
    public:
        void reset() { z1 = z2 = 0.0f; }

        float process (float x)
        {
            const float y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }

        void setLowpass (float fs, float freq)
        {
            const float w0    = 2.0f * 3.14159265358979323846f * freq / fs;
            const float cw    = std::cos (w0);
            const float alpha = std::sin (w0) * 0.5f;   // Q = 0.7071 (Butterworth)
            const float a0    = 1.0f + alpha;
            b0 = (1.0f - cw) * 0.5f / a0;
            b1 = (1.0f - cw) / a0;
            b2 = b0;
            a1 = (-2.0f * cw) / a0;
            a2 = (1.0f - alpha) / a0;
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

    private:
        float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
        float z1 = 0.0f, z2 = 0.0f;
    };

    // One-pole magnitude envelope follower.
    class EnvelopeFollower
    {
    public:
        void setTimes (float fs, float attackMs, float releaseMs)
        {
            attackCoeff  = 1.0f - std::exp (-1.0f / (std::max (0.1f, attackMs)  * 0.001f * fs));
            releaseCoeff = 1.0f - std::exp (-1.0f / (std::max (0.1f, releaseMs) * 0.001f * fs));
        }
        void reset() { env = 0.0f; }

        float process (float x)
        {
            const float a = std::fabs (x);
            env += (a > env ? attackCoeff : releaseCoeff) * (a - env);
            return env;
        }

    private:
        float attackCoeff = 0.0f;
        float releaseCoeff = 0.0f;
        float env = 0.0f;
    };

    // -----------------------------------------------------------------------
    // Multiband splitter (per channel).
    // -----------------------------------------------------------------------
    class MultibandSplitter
    {
    public:
        void reset() { for (auto& f : lp) f.reset(); }

        // Sets `count` lowpasses from `x` (sorted ascending internally).
        void setCrossovers (float fs, const std::array<float, kMaxCrossovers>& x, int count)
        {
            std::array<float, kMaxCrossovers> s = x;
            std::sort (s.begin(), s.end());
            for (int i = 0; i < count && i < kMaxCrossovers; ++i)
                lp[static_cast<size_t> (i)].setLowpass (fs, std::clamp (s[static_cast<size_t> (i)], 20.0f, fs * 0.48f));
        }

        // Writes `bands` signals into `out` (length >= bands). Sum == x.
        void process (float x, float* out, int bands)
        {
            if (bands <= 1)
            {
                out[0] = x;
                return;
            }

            float low[kMaxCrossovers];
            for (int i = 0; i < bands - 1; ++i)
                low[i] = lp[static_cast<size_t> (i)].process (x);

            out[0] = low[0];
            for (int i = 1; i < bands - 1; ++i)
                out[i] = low[i] - low[i - 1];
            out[bands - 1] = x - low[bands - 2];
        }

    private:
        std::array<Biquad, kMaxCrossovers> lp {};
    };

    // -----------------------------------------------------------------------
    // Everything the audio thread needs, mirroring the plugin parameters.
    // -----------------------------------------------------------------------
    struct Params
    {
        int  order = 0;      // 0..5: permutation of {suppressor, OTT, gate}

        struct ModuleParams
        {
            bool  enabled = false;
            int   bands   = 1;        // 1..5
            std::array<float, kMaxCrossovers> xover { 200.0f, 2000.0f, 6000.0f, 10000.0f };

            // Suppressor per-band + global.
            std::array<float, kMaxBands> suppAmount      { 0.5f, 0.5f, 0.5f, 0.5f, 0.5f };
            std::array<float, kMaxBands> suppSensitivity { 0.5f, 0.5f, 0.5f, 0.5f, 0.5f };
            std::array<float, kMaxBands> suppQ           { 0.5f, 0.5f, 0.5f, 0.5f, 0.5f };
            float suppAttackMs = 10.0f;
            float suppReleaseMs = 120.0f;
            float suppSlope    = 0.5f;

            // OTT per-band + global.
            std::array<float, kMaxBands> ottThresholdDb { -30.0f, -30.0f, -30.0f, -30.0f, -30.0f };
            std::array<float, kMaxBands> ottUp          { 0.6f, 0.6f, 0.6f, 0.6f, 0.6f };
            std::array<float, kMaxBands> ottDown        { 0.6f, 0.6f, 0.6f, 0.6f, 0.6f };
            float ottAttackMs  = 5.0f;
            float ottReleaseMs = 80.0f;

            // Gate per-band + global.
            std::array<float, kMaxBands> gateThresholdDb { -40.0f, -40.0f, -40.0f, -40.0f, -40.0f };
            std::array<float, kMaxBands> gateRangeDb     { -60.0f, -60.0f, -60.0f, -60.0f, -60.0f };
            float gateAttackMs  = 2.0f;
            float gateReleaseMs = 60.0f;

            bool operator== (const ModuleParams& o) const
            {
                return enabled == o.enabled
                    && bands   == o.bands
                    && xover   == o.xover
                    && suppAmount      == o.suppAmount
                    && suppSensitivity == o.suppSensitivity
                    && suppQ           == o.suppQ
                    && suppAttackMs  == o.suppAttackMs
                    && suppReleaseMs == o.suppReleaseMs
                    && suppSlope     == o.suppSlope
                    && ottThresholdDb == o.ottThresholdDb
                    && ottUp          == o.ottUp
                    && ottDown        == o.ottDown
                    && ottAttackMs  == o.ottAttackMs
                    && ottReleaseMs == o.ottReleaseMs
                    && gateThresholdDb == o.gateThresholdDb
                    && gateRangeDb     == o.gateRangeDb
                    && gateAttackMs  == o.gateAttackMs
                    && gateReleaseMs == o.gateReleaseMs;
            }
        };
        std::array<ModuleParams, kNumModules> mod;

        float mix    = 1.0f;     // 0..1
        float output = 1.0f;     // 0..2
        bool  autoGain = false;  // gain compensation (off by default)

        float eqFreq[kNumEqBands] = { 120.0f, 250.0f, 500.0f, 1000.0f, 2000.0f, 4000.0f, 8000.0f, 12000.0f };
        float eqGain[kNumEqBands] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };  // dB, 0 = inactive

        bool operator== (const Params& o) const
        {
            if (order != o.order || mix != o.mix || output != o.output || autoGain != o.autoGain || mod != o.mod)
                return false;
            for (int i = 0; i < kNumEqBands; ++i)
                if (eqFreq[i] != o.eqFreq[i] || eqGain[i] != o.eqGain[i])
                    return false;
            return true;
        }
    };

    // -----------------------------------------------------------------------
    // Optional auto makeup gain: matches processed loudness to the dry input.
    // -----------------------------------------------------------------------
    class GainCompensator
    {
    public:
        void prepare (double fs) { coeff = static_cast<float> (1.0 - std::exp (-1.0 / (0.15 * fs))); reset(); }
        void reset() { inLvl = outLvl = 0.0f; gain = 1.0f; }

        float process (float dryMag, float wetMag)
        {
            inLvl  += coeff * (dryMag - inLvl);
            outLvl += coeff * (wetMag - outLvl);

            float target = 1.0f;
            if (outLvl > 1.0e-4f && inLvl > 1.0e-4f)
                target = inLvl / outLvl;

            target = std::clamp (target, 0.25f, 4.0f);   // ±12 dB
            gain += coeff * (target - gain);
            return gain;
        }

    private:
        float coeff = 0.0f;
        float inLvl = 0.0f;
        float outLvl = 0.0f;
        float gain = 1.0f;
    };

    class SpectralSuppressor
    {
    public:
        void prepare (double sampleRate, int maxBlockSize);
        void reset();
        void setParams (const Params& p);
        void process (const float* inL, const float* inR,
                      float* outL, float* outR, int numSamples);

        // ---- visualization -------------------------------------------------
        // Recent mono input for the spectrum display.
        const std::vector<float>& vizInput() const { return vizInputBuf; }
        int  vizWritePos() const                    { return vizWrite.load(); }
        double getSampleRate() const                { return sampleRate; }

        bool  isModuleEnabled (Module m) const;
        int   getModuleBands (Module m) const;             // active band count
        float getModuleBandFreq (Module m, int band) const; // Hz
        float getModuleBandGainDb (Module m, int band) const; // applied gain dB (0 if idle)

    private:
        // ---- resonance suppressor ------------------------------------------
        struct SuppBand { Biquad detect, cut; float freq = 0.0f, env = 0.0f, gainDb = 0.0f, appliedDb = 999.0f; };
        // ---- OTT -----------------------------------------------------------
        struct OttBand { EnvelopeFollower env; float gainDb = 0.0f; };
        // ---- gate ----------------------------------------------------------
        struct GateBand { EnvelopeFollower env; float gainDb = 0.0f; };

        // Per-channel state for one module.
        struct SuppressorState { MultibandSplitter split; std::array<SuppBand, kMaxBands> band; };
        struct OttState          { MultibandSplitter split; std::array<OttBand,  kMaxBands> band; };
        struct GateState         { MultibandSplitter split; std::array<GateBand, kMaxBands> band; };
        struct EqState           { std::array<Biquad, kNumEqBands> band; };

        void update (double sr);
        void processModule (Module m, float& x, int ch);
        float suppressor (Module m, float x, int ch);
        float ott (Module m, float x, int ch);
        float gate (Module m, float x, int ch);

        double sampleRate = 48000.0;
        Params p;
        int orderArr[3] = { 0, 1, 2 };

        float maxCutDb    = 24.0f;
        float suppSlopeFactor = 1.0f;

        // Band centre frequencies (shared per module), computed from crossovers.
        std::array<float, kMaxBands> bandFreq[kNumModules] {};

        std::array<SuppressorState, 2> suppCh;
        std::array<OttState, 2>          ottCh;
        std::array<GateState, 2>         gateCh;
        std::array<EqState, 2>           eqCh;
        GainCompensator agc;

        static constexpr int kVizSize = 4096;
        std::vector<float> vizInputBuf;
        std::atomic<int> vizWrite { 0 };
    };

} // namespace temper
