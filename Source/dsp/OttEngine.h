#pragma once

/*
  Temper — the OTT module: three-band upward + downward compression with MO-TT-style
  controls, on either of two engines.

    Multiband - time domain, sample-accurate: Linkwitz-Riley crossovers (flat sum), RMS or
                peak detection, optional lookahead. The punchy one.
    Spectral  - an FFT (1024 at 48 kHz, 2.7 ms hops) where the bands are linear-phase masks.
                Resolution sets what each bin's gain follows: 0 = its band's level (a
                linear-phase multiband compressor), 1 = its own neighbourhood (a true spectral
                compressor: peaks pulled down, dips lifted, the spectrum densified).

  Per band: Above threshold + Downward amount (1 = inf:1, more = past inf:1), Below threshold
  + Upward amount, input and output gain, Amount, attack / release, solo, bypass. Globally:
  Amount (scales every band's ratios and gains), Time (scales attack / release), input and
  output gain, low / high tone, knee, detection, timing style, stereo link (0..200 %),
  clipper, sidechain (by band or wideband). JUCE-free.
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
        float computeGain (const OttBand& b, float levelDb, float amt) const;
        void processMultiband (float& l, float& r, float scl, float scr);
        void spectralFrame (const StereoStft& s, std::array<std::vector<float>, 2>& gain);
        void updateViz();

        double sr = 48000.0;
        OttSettings st;
        bool prepared = false;

        // Multiband.
        std::array<Lr4Split, 2> split, scSplit;
        std::array<std::array<DelayLine, kDynBands>, 2> look;
        std::array<std::array<float, kDynBands>, 2> ms {}, gDb {}, gDb2 {};
        float rmsCoef = 0.0f;
        int lookSamples = 0;
        std::array<float, kDynBands> attC {}, relC {};
        DelayLine offDelayL, offDelayR;

        // Spectral.
        StereoStft stft;
        int specN = 0;
        std::vector<float> binFreq;
        std::array<std::vector<float>, 2> levelBuf;   // per-bin detector level (dB)
        int vizCount = 0;
        std::array<std::vector<float>, kDynBands> mask;
        std::array<float, kDynBands> maskBins {};
        std::array<std::vector<float>, 2> specG, specG2;
        std::vector<double> prefix;
        std::vector<int> halfW;

        std::array<std::atomic<float>, kCurvePoints> viz {};
        std::array<std::atomic<float>, kDynBands> levelViz {}, gainViz {};
    };
} // namespace temper
