#pragma once

/*
  Temper — modular spectral processor.

  Three modules run in a user-reorderable serial chain, followed by an 8-node manual EQ:

    1. Resonance suppressor  — a dynamic spectral equalizer (SpectralDynamics.h): every FFT
       bin is its own dynamic EQ, DSEQ-style global controls (threshold, slope, selectivity,
       attack / release, strength, max GR, link), detector pre-filters, sidechain, custom
       threshold curve, linear or natural phase.
    2. OTT                   — three-band upward + downward compression, MO-TT-style controls,
       on a time-domain multiband engine or a true spectral one (OttEngine.h).
    3. Gate                  — band gate / expander or a per-bin spectral gate with a learned
       noise profile (GateEngine.h).

  Each module may delay the signal (FFT engines, lookahead); the latencies add up, the dry
  path and the sidechain are delayed to match, and the plugin reports the total.

  Manual EQ: bell, shelves, cuts (12 / 24 / 48 dB/oct), notch, band pass, tilt; each band on
  stereo, left, right, mid or side; band listen.
*/

#include "Filters.h"
#include "SpectralDynamics.h"
#include "OttEngine.h"
#include "GateEngine.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <vector>

namespace temper
{
    static constexpr int kNumModules    = 3;
    static constexpr int kNumEqBands    = 8;

    enum class Module : int { Suppressor = 0, Ott = 1, Gate = 2 };
    enum class Listen : int { Off = 0, Delta = 1, Detector = 2 };

    // -----------------------------------------------------------------------
    // Everything the audio thread needs, mirroring the plugin parameters.
    // -----------------------------------------------------------------------
    struct Params
    {
        int  order = 0;      // 0..5: permutation of {suppressor, OTT, gate}

        DynSettings  dyn;                             // resonance module
        OttSettings  ott;
        GateSettings gate;

        float mix    = 1.0f;     // 0..1
        float output = 1.0f;     // 0..2
        bool  autoGain = false;
        int   listen = 0;        // Listen
        int   eqListen = 0;      // 0 off, 1..8: hear only that EQ band's region of the output

        std::array<FilterNode, kNumEqBands> eq {};

        bool operator== (const Params& o) const
        {
            return order == o.order && mix == o.mix && output == o.output && autoGain == o.autoGain
                && listen == o.listen && eqListen == o.eqListen && dyn == o.dyn && ott == o.ott && gate == o.gate && eq == o.eq;
        }
    };

    // Optional auto makeup gain: matches processed loudness to the (delayed) dry input.
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
            target = std::clamp (target, 0.25f, 4.0f);   // +-12 dB
            gain += coeff * (target - gain);
            return gain;
        }

    private:
        float coeff = 0.0f, inLvl = 0.0f, outLvl = 0.0f, gain = 1.0f;
    };

    class SpectralSuppressor
    {
    public:
        void prepare (double sampleRate, int maxBlockSize);
        void reset();
        void setParams (const Params& p);
        // scL / scR: the sidechain (null => the main input).
        void process (const float* inL, const float* inR, const float* scL, const float* scR,
                      float* outL, float* outR, int numSamples);

        int getLatency() const { return dyn.latency() + ott.latency() + gate.latency(); }
        SpectralDynamics& dynamics() { return dyn; }
        const SpectralDynamics& dynamics() const { return dyn; }
        OttEngine& ottEngine() { return ott; }
        const OttEngine& ottEngine() const { return ott; }
        GateEngine& gateEngine() { return gate; }
        const GateEngine& gateEngine() const { return gate; }

        // ---- visualization -------------------------------------------------
        const std::vector<float>& vizInput() const { return vizInputBuf; }
        const std::vector<float>& vizOutputL() const { return vizOutL; }
        const std::vector<float>& vizOutputR() const { return vizOutR; }
        int  vizWritePos() const                    { return vizWrite.load(); }
        double getSampleRate() const                { return sampleRate; }

        bool  isModuleEnabled (Module m) const;
        // Gain (dB) a module applies at display point p (kCurvePoints, 20 Hz..20 kHz).
        float moduleGainAt (Module m, int p) const;

    private:
        struct EqBand    { std::array<Biquad2, 4> f[2]; int count = 0; };

        void update (double sr);

        double sampleRate = 48000.0;
        Params p;
        int orderArr[3] = { 0, 1, 2 };

        SpectralDynamics dyn;
        OttEngine  ott;
        GateEngine gate;
        std::array<EqBand, kNumEqBands> eqBands;
        std::array<Biquad2, 2> listenFilter;
        GainCompensator agc;

        // The dry signal and the sidechain, delayed to line up with each stage's input.
        std::array<std::vector<float>, 2> dryDelay, scDelay;
        int dryPos = 0;

        static constexpr int kVizSize = 4096;
        std::vector<float> vizInputBuf;
        std::vector<float> vizOutL = std::vector<float> (kVizSize, 0.0f);
        std::vector<float> vizOutR = std::vector<float> (kVizSize, 0.0f);
        std::atomic<int> vizWrite { 0 };
    };

} // namespace temper
