#pragma once

/*
  Temper — the resonance module: a dynamic spectral equalizer (DSEQ-style).

  Short-time Fourier transform, Hann windows at 8x overlap, FFT size set by Quality
  (2048 / 4096 / 8192 at 48 kHz, scaled with the sample rate). Every bin is its own dynamic
  EQ: its detector level (the main input or the sidechain, weighted by the pre-filters and a
  dB/oct slope around 1 kHz) is compared with a threshold, and the excess becomes gain
  reduction (Strength x excess, soft knee, capped by Max GR). Selectivity sets how far a
  cut spreads to neighbouring bins; Attack / Release smooth each bin over time; Link joins
  the two channels. The gain is applied with zero phase (Linear) or as a minimum-phase
  filter built from it by the cepstrum (Natural: no pre-ringing, a little phasing).

  Threshold modes:
    Fixed     - Threshold is a level (a full-scale sine reads 0 dB in its bin).
    Adaptive  - Threshold is dB above the music's own long-term spectrum (smoothed over an
                octave), so only what sticks out of its surroundings is reduced, at any level.
    Custom    - Threshold plus a curve learned from the input (Learn), normalised to 0 dB.

  Latency is the FFT size. Left/right or mid/side. JUCE-free.
*/

#include "Fft.h"
#include "Filters.h"

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

    class SpectralDynamics
    {
    public:
        void prepare (double sampleRate);
        void reset();
        void setSettings (const DynSettings& s);
        void setMonitorDetector (bool on) { monitorDet = on; }

        int latency() const { return n; }
        static int fftSizeFor (int quality, double sampleRate);

        // One stereo sample in place (delayed by latency()); sc = the sidechain sample pair.
        void process (float& l, float& r, float scl, float scr);

        // ---- custom threshold (Learn) ----------------------------------------------------
        void startLearn()             { learnRequest.store (1); }
        void stopLearn()              { learnRequest.store (2); }
        bool isLearning() const       { return learning.load(); }
        bool hasCustomCurve() const   { return curveValid.load(); }
        void getCustomCurve (float* dB) const;          // kCurvePoints values
        void setCustomCurve (const float* dB);          // any thread: picked up by the next frame
        void clearCustomCurve()       { clearRequest.store (true); }

        // ---- display ---------------------------------------------------------------------
        float grAt (int point) const          { return grViz[static_cast<size_t> (point)].load (std::memory_order_relaxed); }
        float currentSlope() const            { return slopeViz.load (std::memory_order_relaxed); }
        float maxGrNow() const                { return maxGrViz.load (std::memory_order_relaxed); }

    private:
        void configure();                     // FFT size, window, per-bin tables; clears state
        void frame();
        void rebuildPreWeights();
        void rebuildCustomBins();
        void rebuildSpread();
        void spread (float* g, float* tmp) const;   // selectivity across frequency

        double sr = 48000.0;
        DynSettings st;
        bool monitorDet = false;

        int n = 0, hop = 0, half = 0, maxN = 0;
        Fft fft;
        std::vector<float> window, binFreq, log2f;
        float olaScale = 1.0f;

        // Time-domain rings (size n) and overlap-add accumulators.
        std::array<std::vector<float>, 2> inBuf, scBuf, outAcc;
        int wpos = 0, rpos = 0, hopCount = 0;

        // Spectral work arrays.
        std::vector<float> re, im, sre, sim;
        std::array<std::vector<float>, 2> xr, xi, det, gr, grS, gainRe, gainIm;
        std::vector<float> tmpA, tmpB, ltAvg, adaptCurve, preW, customBin;
        std::vector<int>   spreadHalf;     // per-bin half-width for the current selectivity
        std::vector<int>   dqBuf;          // spread(): the sliding-minimum deque
        std::vector<double> prefix;        // running sums (spread, adaptive curve)
        std::vector<float> ltPow;          // long-term power per bin
        std::array<std::vector<float>, 2> detPow;   // smoothed detector power
        bool ltInit = false, detInit = false;
        float tilt = 0.0f;

        // Custom curve.
        std::array<float, kCurvePoints> curve {}, pendingCurve {};
        std::atomic<bool> curveValid { false }, pendingSet { false }, clearRequest { false };
        std::atomic<int>  learnRequest { 0 };
        std::atomic<bool> learning { false };
        std::vector<double> learnSum;
        int learnFrames = 0;

        // Display.
        std::array<std::atomic<float>, kCurvePoints> grViz {};
        std::array<int, kCurvePoints + 1> vizBin {};
        std::atomic<float> slopeViz { 0.0f }, maxGrViz { 0.0f };
    };
} // namespace temper
