#include "SpectralSuppressor.h"

namespace temper
{
    void SpectralSuppressor::prepare (double sr, int /*maxBlockSize*/)
    {
        sampleRate = sr;
        vizInputBuf.assign (kVizSize, 0.0f);
        vizWrite.store (0);
        reset();
        update (sr);
    }

    void SpectralSuppressor::reset()
    {
        for (auto& ch : bands)
            for (auto& b : ch)
            {
                b.env       = 0.0f;
                b.gainDb    = 0.0f;
                b.appliedDb = 999.0f;
                b.detect.reset();
                b.cut.reset();
            }

        for (auto& ch : eq)
            for (auto& e : ch)
                e.filter.reset();

        vizWrite.store (0);
    }

    void SpectralSuppressor::setParams (const Params& np)
    {
        if (np == p)
            return;
        p = np;
        update (sampleRate);
    }

    void SpectralSuppressor::update (double sr)
    {
        const float fs = static_cast<float> (sr);

        attCoef = 1.0f - std::exp (-1.0f / (std::max (1.0f, p.attackMs)  * 0.001f * fs));
        relCoef = 1.0f - std::exp (-1.0f / (std::max (1.0f, p.releaseMs) * 0.001f * fs));

        // Sensitivity 0..1 maps to a threshold of -10 .. -50 dBFS.
        const float thresholdDb = -10.0f - p.sensitivity * 40.0f;
        thresholdLin = std::pow (10.0f, thresholdDb / 20.0f);

        // Slope scales the suppression aggressiveness; Q sets the bandwidth.
        slopeFactor = 0.5f + 1.5f * p.slope;
        qCut        = 1.0f + 5.0f * (1.0f - p.q);   // 6 (narrow) .. 1 (wide)
        qDetect     = 0.8f + 1.2f * (1.0f - p.q);

        // Log-spaced dynamic band centres across the resonance region.
        const float f0 = 120.0f, f1 = 12000.0f;
        for (int i = 0; i < kNumBands; ++i)
        {
            const float t    = (kNumBands > 1) ? static_cast<float> (i) / (kNumBands - 1) : 0.0f;
            const float freq = f0 * std::pow (f1 / f0, t);

            for (auto& ch : bands)
            {
                auto& b    = ch[static_cast<size_t> (i)];
                b.freq     = freq;
                b.detect.setBandpass (fs, freq, qDetect);
                b.cut.setPeaking    (fs, freq, qCut, b.gainDb);
                b.appliedDb = b.gainDb;
            }
        }

        // Manual EQ nodes (fixed Q = 1.0).
        for (int i = 0; i < kNumEqBands; ++i)
        {
            const float freq = std::clamp (p.eqFreq[static_cast<size_t> (i)], 40.0f, fs * 0.45f);
            const float gain = std::clamp (p.eqGain[static_cast<size_t> (i)], -18.0f, 18.0f);

            for (auto& ch : eq)
            {
                auto& e = ch[static_cast<size_t> (i)];
                e.freq   = freq;
                e.gainDb = gain;
                e.filter.setPeaking (fs, freq, 1.0f, gain);
            }
        }
    }

    float SpectralSuppressor::getReductionDb (int band) const
    {
        return (band >= 0 && band < kNumBands) ? bands[0][static_cast<size_t> (band)].gainDb : 0.0f;
    }

    void SpectralSuppressor::process (const float* inL, const float* inR,
                                      float* outL, float* outR, int numSamples)
    {
        const float fs     = static_cast<float> (sampleRate);
        const float amount = p.amount;
        const float mix    = p.mix;
        const float output = p.output;

        for (int s = 0; s < numSamples; ++s)
        {
            // Ring buffer of mono input for the spectrum display.
            const float mono = 0.5f * (inL[s] + inR[s]);
            const int w = vizWrite.load();
            vizInputBuf[static_cast<size_t> (w)] = mono;
            vizWrite.store ((w + 1) % kVizSize);

            for (int ch = 0; ch < 2; ++ch)
            {
                const float x = (ch == 0) ? inL[s] : inR[s];
                auto& chBands = bands[ch];

                // 1) Detect each band's energy and derive its target gain.
                for (auto& b : chBands)
                {
                    const float d = std::fabs (b.detect.process (x));
                    if (d > b.env) b.env = d;
                    else           b.env += relCoef * (d - b.env);

                    float target = 0.0f;
                    if (b.env > thresholdLin)
                    {
                        const float overDb = 20.0f * std::log10 (b.env / thresholdLin);
                        target = std::clamp (-amount * overDb * slopeFactor, -maxCutDb, 0.0f);
                    }

                    const float coef = (target < b.gainDb) ? attCoef : relCoef;
                    b.gainDb += coef * (target - b.gainDb);

                    if (std::fabs (b.gainDb - b.appliedDb) > 0.05f)
                    {
                        b.cut.setPeaking (fs, b.freq, qCut, b.gainDb);
                        b.appliedDb = b.gainDb;
                    }
                }

                // 2) Dynamic suppression path.
                float y = x;
                for (auto& b : chBands)
                    y = b.cut.process (y);

                // 3) Manual EQ path.
                for (auto& e : eq[ch])
                    y = e.filter.process (y);

                const float out = std::clamp ((x * (1.0f - mix) + y * mix) * output, -1.0f, 1.0f);
                if (ch == 0) outL[s] = out; else outR[s] = out;
            }
        }
    }

} // namespace temper
