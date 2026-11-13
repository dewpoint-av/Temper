#pragma once

/*
  Temper — the gate module, on either of two engines.

    Band     - time domain, 1..3 Linkwitz-Riley bands: per band threshold, range and ratio
               (gate .. gentle expander), with hysteresis, hold, attack / release, lookahead,
               flip (duck instead of gate), sidechain and sidechain high / low-pass filters
               (on what triggers the gate). Stereo-linked.
    Spectral - every FFT bin is its own gate (2048 at 48 kHz). Thresholds come from the bands;
               against a noise reference they are dB above the noise in each bin, which makes
               it a noise / bleed remover. The reference is either learned (Learn on a noise-only
               part) or tracked automatically (minimum statistics: the floor each bin keeps
               returning to over the last 1.5 s, so it follows changing noise with no learn step).
               The sidechain filters set which frequencies are gated. Smoothing spreads each
               bin's gain over its neighbours against "musical noise".

  JUCE-free.
*/

#include "Bands.h"
#include "SpectralDynamics.h"   // kCurvePoints, curvePointFreq
#include "StereoStft.h"

#include <array>
#include <atomic>
#include <vector>

namespace temper
{
    struct GateBand
    {
        float thrDb = -40.0f, rangeDb = -60.0f, ratio = 100.0f;   // ratio >= 50 acts as a gate
        bool operator== (const GateBand& o) const { return thrDb == o.thrDb && rangeDb == o.rangeDb && ratio == o.ratio; }
    };

    struct GateSettings
    {
        bool  enabled = false;
        int   engine = 0;          // 0 band, 1 spectral
        int   bands = 1;           // 1..3
        float x1 = 200.0f, x2 = 4000.0f;
        float attMs = 1.0f, holdMs = 20.0f, relMs = 100.0f, hystDb = 4.0f, lookMs = 0.0f;
        bool  flip = false;
        bool  sidechain = false;
        float scHpf = 20.0f, scLpf = 20000.0f;
        float smoothing = 0.5f;    // spectral
        int   profileMode = 0;     // spectral noise reference: 0 none (thresholds are levels), 1 learned, 2 auto-tracked
        std::array<GateBand, kDynBands> band {};
        bool operator== (const GateSettings& o) const
        {
            return enabled == o.enabled && engine == o.engine && bands == o.bands && x1 == o.x1 && x2 == o.x2 && attMs == o.attMs
                && holdMs == o.holdMs && relMs == o.relMs && hystDb == o.hystDb && lookMs == o.lookMs && flip == o.flip
                && sidechain == o.sidechain && scHpf == o.scHpf && scLpf == o.scLpf && smoothing == o.smoothing
                && profileMode == o.profileMode && band == o.band;
        }
        bool operator!= (const GateSettings& o) const { return ! (*this == o); }
    };

    class GateEngine
    {
    public:
        void prepare (double sampleRate);
        void reset();
        void setSettings (const GateSettings& s);
        int latency() const;
        static int maxLatency (double sampleRate);
        void process (float& l, float& r, float scl, float scr);

        // Noise profile (spectral engine).
        void startLearn()             { learnRequest.store (1); }
        void stopLearn()              { learnRequest.store (2); }
        bool isLearning() const       { return learning.load(); }
        bool hasProfile() const       { return profileValid.load(); }
        void getProfile (float* dB) const;
        void setProfile (const float* dB);
        void clearProfile()           { clearRequest.store (true); }
        void getAutoProfile (float* dB) const;   // the tracked floor, on kCurvePoints (display)

        float gainAt (int p) const          { return viz[static_cast<size_t> (p)].load (std::memory_order_relaxed); }
        float bandGainDb (int b) const      { return gainViz[static_cast<size_t> (b)].load (std::memory_order_relaxed); }

    private:
        float targetGain (const GateBand& b, float levelDb, bool open) const;
        void processBand (float& l, float& r, float scl, float scr);
        void spectralFrame (const StereoStft& s, std::array<std::vector<float>, 2>& gain);
        void rebuildMasks();

        double sr = 48000.0;
        GateSettings st;
        bool prepared = false;

        // Band engine.
        std::array<Lr4Split, 2> split, detSplit;
        std::array<std::array<Biquad2, 2>, 2> scFilt;   // [channel][hp, lp]
        std::array<std::array<DelayLine, kDynBands>, 2> look;
        std::array<float, kDynBands> env {}, gDb {}, envDecay {};
        std::array<bool, kDynBands> open {};
        std::array<int, kDynBands> hold {};
        float attC = 0, relC = 0;
        int holdSamples = 0, lookSamples = 0;
        DelayLine offL, offR;

        // Spectral engine.
        StereoStft stft;
        int specN = 0;
        std::vector<float> binFreq, scWeight, profileBin, level, gSpec, gTmp, holdF;
        std::vector<unsigned char> openBin;
        // Auto noise reference (minimum statistics): smoothed power, the current sub-window's
        // minimum, the last sub-windows' minima, and the resulting floor in dB per bin.
        static constexpr int kAutoSub = 8;
        std::vector<float> autoS, autoSubMin, autoBin;
        std::array<std::vector<float>, kAutoSub> autoHist;
        int autoFrames = 0, autoSubLen = 1, autoHistPos = 0, autoHistCount = 0;
        bool autoInit = false;
        std::array<std::atomic<float>, kCurvePoints> autoViz {};
        std::array<std::vector<float>, kDynBands> mask;
        std::array<float, kDynBands> maskBins {};
        std::array<float, kCurvePoints> profile {}, pendingProfile {};
        std::atomic<bool> profileValid { false }, pendingSet { false }, clearRequest { false }, learning { false };
        std::atomic<int> learnRequest { 0 };
        std::vector<double> learnSum;
        int learnFrames = 0;

        std::array<std::atomic<float>, kCurvePoints> viz {};
        std::array<std::atomic<float>, kDynBands> gainViz {};
        int vizCount = 0;
    };
} // namespace temper
