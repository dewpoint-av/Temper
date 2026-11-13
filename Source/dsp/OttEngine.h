#pragma once

/*
  Temper — the OTT module: three-band upward + downward compression with MO-TT-style
  controls, on either of two engines.

    Multiband - time domain, sample-accurate: Linkwitz-Riley crossovers (flat sum), per-band
                two-pole RMS (or peak) detectors timed to the band (a sub tone reads as a steady
                level, not a ripple, so pushing the low band hard stays clean), optional
                lookahead with gain ramping. The punchy one.
    Spectral  - two FFT resolutions: 4096 (at 48 kHz) below ~250 Hz for the sub and low band,
                1024 (2.7 ms hops) above for the mids and highs, crossed over with linear-phase
                masks; the bands are linear-phase masks too. Resolution sets what each bin's
                gain follows: 0 = its band's level (a linear-phase multiband compressor), 1 = its
                own neighbourhood (a true spectral compressor: peaks pulled down, dips lifted).
                The gain curve's slope across bins is limited so the equivalent filter stays
                short (no time-aliasing grit). Latency = 4096.

  Per band: Above threshold + Downward amount (1 = inf:1, more = past inf:1), Below threshold
  + Upward amount, input and output gain, Amount, attack / release, solo, bypass. Globally:
  Amount (scales every band's ratios and gains), Time (scales attack / release), input and
  output gain, low / high tone, knee, detection, timing style, stereo link (0..200 %),
  an anti-aliased output clipper, sidechain (by band or wideband). JUCE-free.
*/

#include "Bands.h"
#include "SpectralDynamics.h"   // kCurvePoints, curvePointFreq
#include "StereoStft.h"

#include <array>
#include <atomic>
#include <vector>

namespace temper
{
    struct OttBand
    {
        float aboveDb = -30.0f, down = 1.0f;     // down: 0 = 1:1, 1 = inf:1, > 1 = past inf:1
        float belowDb = -40.0f, up = 0.76f;      // up: gain = up x how far below
        float inDb = 5.2f, outDb = 0.0f, amount = 1.0f;
        float attMs = 20.0f, relMs = 280.0f;
        bool  solo = false, bypass = false;
        bool operator== (const OttBand& o) const
        {
            return aboveDb == o.aboveDb && down == o.down && belowDb == o.belowDb && up == o.up && inDb == o.inDb
                && outDb == o.outDb && amount == o.amount && attMs == o.attMs && relMs == o.relMs && solo == o.solo && bypass == o.bypass;
        }
    };

    struct OttSettings
    {
        bool  enabled = false;
        int   engine = 0;          // 0 multiband, 1 spectral
        int   style = 0;           // 0 classic, 1 smooth, 2 smack
        bool  softKnee = true;
        bool  rms = true;          // multiband detection (spectral is always RMS-like)
        float amount = 1.0f, time = 1.0f, inDb = 0.0f, outDb = 0.0f, lowTone = 0.0f, highTone = 0.0f;
        float link = 1.0f;         // 0..2
        float resolution = 0.5f;   // spectral: 0 band .. 1 bin
        float lookaheadMs = 0.0f;  // multiband
        bool  clip = false;
        float clipDb = 0.0f;
        bool  sidechain = false;
        bool  scWide = false;
        float x1 = 88.3f, x2 = 2500.0f;
        std::array<OttBand, kDynBands> band {};
        bool operator== (const OttSettings& o) const
        {
            return enabled == o.enabled && engine == o.engine && style == o.style && softKnee == o.softKnee && rms == o.rms
                && amount == o.amount && time == o.time && inDb == o.inDb && outDb == o.outDb && lowTone == o.lowTone
                && highTone == o.highTone && link == o.link && resolution == o.resolution && lookaheadMs == o.lookaheadMs
                && clip == o.clip && clipDb == o.clipDb && sidechain == o.sidechain && scWide == o.scWide && x1 == o.x1 && x2 == o.x2
                && band == o.band;
        }
        bool operator!= (const OttSettings& o) const { return ! (*this == o); }
    };

    class OttEngine
    {
    public:
        void prepare (double sampleRate);
        void reset();
        void setSettings (const OttSettings& s);
        int latency() const;                              // samples (engine-dependent, also when off)
        static int maxLatency (double sampleRate);
        void process (float& l, float& r, float scl, float scr);

        // Display: compression gain (dB) at display point p (kCurvePoints, 20 Hz..20 kHz), and
        // each band's current level in dBFS (left channel or the louder).
        float gainAt (int p) const        { return viz[static_cast<size_t> (p)].load (std::memory_order_relaxed); }
        float bandLevelDb (int b) const   { return levelViz[static_cast<size_t> (b)].load (std::memory_order_relaxed); }
        float bandGainDb (int b) const    { return gainViz[static_cast<size_t> (b)].load (std::memory_order_relaxed); }

    private:
       #ifndef TEMPER_OTT_RES_SPLIT
        #define TEMPER_OTT_RES_SPLIT 250.0f
       #endif
        static constexpr float kResSplitHz = TEMPER_OTT_RES_SPLIT;   // spectral: long FFT below, short above (the define: tests)

        struct SpecRes
        {
            StereoStft stft;
            int n = 0;
            std::vector<float> binFreq, share;                       // share: this resolution's weight per bin
            std::array<std::vector<float>, kDynBands> mask;
            std::array<float, kDynBands> maskBins {};
            std::vector<int> halfW;
            std::array<std::vector<float>, 2> level, g1, g2, gOut;   // per bin: detector dB, smoothed GR stages, final dynamic gain dB
            std::vector<double> prefix;
            float slopeDbPerBin = 6.0f;
            int kLo = 1;                                             // first bin at or above 20 Hz
            int kHi = 0;                                             // last bin with any share (the long one stops ~500 Hz)
        };

        float computeGain (const OttBand& b, float levelDb, float amt) const;
        void processMultiband (float& l, float& r, float scl, float scr);
        void spectralFrame (SpecRes& R, const StereoStft& s, std::array<std::vector<float>, 2>& gain);
        void spectralViz();
        void updateViz();

        double sr = 48000.0;
        OttSettings st;
        bool prepared = false;

        // Multiband.
        std::array<Lr4Split, 2> split, scSplit;
        std::array<std::array<LevelDetector, kDynBands>, 2> det;
        std::array<std::array<DelayLine, kDynBands>, 2> look;
        std::array<std::array<RunningMean, kDynBands>, 2> lookSmooth;
        std::array<std::array<float, kDynBands>, 2> gDb {}, gDb2 {};
        int lookSamples = 0;
        std::array<float, kDynBands> attC {}, relC {};
        DelayLine offDelayL, offDelayR;
        std::array<AdaaClipper, 2> clipper;

        // Spectral.
        std::array<SpecRes, 2> res;                  // 0 long, 1 short
        std::array<DelayLine, 2> shortIn, shortSc;   // the short path waits for the long one
        int vizCount = 0;

        std::array<std::atomic<float>, kCurvePoints> viz {};
        std::array<std::atomic<float>, kDynBands> levelViz {}, gainViz {};
    };
} // namespace temper
