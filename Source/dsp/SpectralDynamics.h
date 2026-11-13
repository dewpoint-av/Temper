#pragma once

/*
  Temper — the resonance module: a dynamic spectral equalizer (DSEQ-style).

  Two short-time Fourier transforms at 8x overlap share the spectrum: a long one (FFT size set
  by Quality: 2048 / 4096 / 8192 at 48 kHz, Blackman-Harris window) for everything below about
  1.5 kHz, where resonances are narrow and the sub needs a leakage-free analysis, and one four
  times shorter (Hann) above it, where transients need time resolution more than frequency
  resolution. They cross over two octaves wide with linear-phase shares that sum to one.

  Every bin is its own dynamic EQ: its detector level (the main input or the sidechain,
  weighted by the pre-filters and a dB/oct slope around 1 kHz) is compared with a threshold,
  and the excess becomes gain reduction (Strength x excess, soft knee, eased into Max GR).
  Selectivity sets how far a cut spreads to neighbouring bins; Attack / Release smooth each
  bin over time; Link joins the two channels. The gain is applied with zero phase (Linear) or
  as a minimum-phase filter built from it by the cepstrum (Natural: no pre-ringing).

  Threshold modes:
    Fixed     - Threshold is a level (a full-scale sine reads 0 dB in its bin).
    Adaptive  - Threshold is dB above the music's own long-term spectrum (smoothed over an
                octave), so only what sticks out of its surroundings is reduced, at any level.
    Custom    - Threshold plus a curve learned from the input (Learn), normalised to 0 dB.

  Off is an exact delay (the transforms rest); switching fades over the latency. Latency is
  the long FFT size. Left/right or mid/side. JUCE-free.
*/

#include "Fft.h"
#include "Filters.h"
#include "Bands.h"

#include <array>
#include <atomic>
#include <vector>

namespace temper
{
    static constexpr int kNumPreFilters = 6;
    static constexpr int kCurvePoints   = 256;    // log-spaced 20 Hz .. 20 kHz: custom curve + display

    inline float curvePointFreq (int i) { return 20.0f * std::pow (1000.0f, static_cast<float> (i) / static_cast<float> (kCurvePoints - 1)); }

    struct DynSettings
    {
        bool  enabled      = true;
        int   quality      = 1;       // 0 normal, 1 high, 2 ultra
        bool  naturalPhase = false;
        bool  midSide      = false;
        int   thrMode      = 1;       // 0 fixed, 1 adaptive, 2 custom
        float thresholdDb  = 6.0f;
        float slopeDbOct   = 0.0f;
        bool  adaptiveSlope = false;
        float selectivity  = 0.5f;    // 0..1
        float attackMs     = 10.0f;
        float releaseMs    = 150.0f;
        float strength     = 1.0f;    // x the excess (1 = down to the threshold)
        float maxGrDb      = 12.0f;
        float link         = 0.75f;   // 0..1
        bool  sidechain    = false;
        float scGainDb     = 0.0f;
        std::array<FilterNode, kNumPreFilters> pre {};

        bool operator== (const DynSettings& o) const
        {
            return enabled == o.enabled && quality == o.quality && naturalPhase == o.naturalPhase && midSide == o.midSide
                && thrMode == o.thrMode && thresholdDb == o.thresholdDb && slopeDbOct == o.slopeDbOct
                && adaptiveSlope == o.adaptiveSlope && selectivity == o.selectivity && attackMs == o.attackMs
                && releaseMs == o.releaseMs && strength == o.strength && maxGrDb == o.maxGrDb && link == o.link
                && sidechain == o.sidechain && scGainDb == o.scGainDb && pre == o.pre;
        }
        bool operator!= (const DynSettings& o) const { return ! (*this == o); }
    };

    // One resolution of the resonance engine: a stereo STFT that analyses the whole spectrum
    // and applies its gain reduction to its share of it.
    class DynCore
    {
    public:
        void prepare (int maxSize, double sampleRate);
        void configure (int size, int windowType, bool lowPart, float splitHz);   // clears state
        void setSettings (const DynSettings& s);
        void setMonitorDetector (bool on) { monitorDet = on; }
        void reset();
        int size() const { return n; }

        void process (float& l, float& r, float scl, float scr);   // in place: this core's share of the output

        void startLearn()             { learnRequest.store (1); }
        void stopLearn()              { learnRequest.store (2); }
        bool isLearning() const       { return learning.load(); }
        bool hasCustomCurve() const   { return curveValid.load(); }
        void getCustomCurve (float* dB) const;
        void setCustomCurve (const float* dB);
        void clearCustomCurve()       { clearRequest.store (true); }
        bool takeNewCurve()           { return newCurve.exchange (false); }   // once after a learn finishes (audio thread)

        float grAt (int point) const          { return grViz[static_cast<size_t> (point)].load (std::memory_order_relaxed); }
        float currentSlope() const            { return slopeViz.load (std::memory_order_relaxed); }
        float maxGrNow() const                { return maxGrViz.load (std::memory_order_relaxed); }

    private:
        void frame();
        void rebuildPreWeights();
        void rebuildCustomBins();
        void rebuildSpread();
        void spread (float* g, float* tmp) const;   // selectivity across frequency

        double sr = 48000.0;
        DynSettings st;
        bool monitorDet = false;

        int n = 0, hop = 0, half = 0, maxN = 0;
        int kLo = 1, kHi = 0;                  // bins this core may act on (20 Hz .. 22 kHz, and its share)
        int kShareLo = 0, kShareHi = 0;        // bins where the share is non-zero
        Fft fft;
        std::vector<float> window, binFreq, log2f, share;
        float olaScale = 1.0f, powNorm = 1.0f;

        std::array<std::vector<float>, 2> inBuf, scBuf, outAcc;
        int wpos = 0, rpos = 0, hopCount = 0;

        std::vector<float> re, im, sre, sim;
        std::array<std::vector<float>, 2> xr, xi, det, gr, grS, gainRe, gainIm;
        std::vector<float> tmpA, tmpB, ltAvg, adaptCurve, preW, customBin;
        std::vector<int>   spreadHalf;
        std::vector<int>   dqBuf;
        std::vector<double> prefix;
        std::vector<float> ltPow;
        std::array<std::vector<float>, 2> detPow;
        bool ltInit = false, detInit = false;
        float tilt = 0.0f;

        std::array<float, kCurvePoints> curve {}, pendingCurve {};
        std::atomic<bool> curveValid { false }, pendingSet { false }, clearRequest { false }, newCurve { false };
        std::atomic<int>  learnRequest { 0 };
        std::atomic<bool> learning { false };
        std::vector<double> learnSum;
        int learnFrames = 0;

        std::array<std::atomic<float>, kCurvePoints> grViz {};
        std::array<int, kCurvePoints + 1> vizBin {};
        std::atomic<float> slopeViz { 0.0f }, maxGrViz { 0.0f };
    };

    class SpectralDynamics
    {
    public:
        void prepare (double sampleRate);
        void reset();
        void setSettings (const DynSettings& s);
        void setMonitorDetector (bool on);

        int latency() const { return n; }
        static int fftSizeFor (int quality, double sampleRate);   // the long (latency-setting) size

        // One stereo sample in place (delayed by latency()); sc = the sidechain sample pair.
        void process (float& l, float& r, float scl, float scr);

        // ---- custom threshold (Learn) ----------------------------------------------------
        void startLearn()             { core[0].startLearn(); core[1].startLearn(); }
        void stopLearn()              { core[0].stopLearn(); core[1].stopLearn(); }
        bool isLearning() const       { return core[0].isLearning(); }
        bool hasCustomCurve() const   { return core[0].hasCustomCurve(); }
        void getCustomCurve (float* dB) const { core[0].getCustomCurve (dB); }   // kCurvePoints values
        void setCustomCurve (const float* dB) { core[0].setCustomCurve (dB); core[1].setCustomCurve (dB); }
        void clearCustomCurve()       { core[0].clearCustomCurve(); core[1].clearCustomCurve(); }

        // ---- display ---------------------------------------------------------------------
        float grAt (int point) const;
        float currentSlope() const            { return core[0].currentSlope(); }
        float maxGrNow() const                { return std::min (core[0].maxGrNow(), core[1].maxGrNow()); }

    private:
        static constexpr float kSplitHz = 1500.0f;
        static int shortSizeFor (int longSize, double sampleRate);
        void configure();

        double sr = 48000.0;
        DynSettings st;
        bool monitorDet = false;
        int n = 0, nShort = 0, maxN = 0;

        std::array<DynCore, 2> core;                      // 0 long (lows), 1 short (highs)
        std::array<DelayLine, 2> dry, shortIn, shortSc;   // exact dry path; the short core waits for the long one
        std::array<float, kCurvePoints> vizLowShare {};

        // Off = the dry delay. Turning on starts the transforms and fades to them once warm.
        bool active = false;
        int warm = 0;
        float xf = 0.0f;
    };
} // namespace temper
