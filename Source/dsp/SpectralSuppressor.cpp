#include "SpectralSuppressor.h"

namespace temper
{
    namespace
    {
        // Band centre frequency from adjacent crossovers (geometric mean).
        float bandCenter (const std::array<float, kMaxCrossovers>& x, int bands, int band)
        {
            const float lo = (band == 0)        ? 20.0f : x[static_cast<size_t> (band - 1)];
            const float hi = (band == bands - 1) ? 20000.0f : x[static_cast<size_t> (band)];
            return std::sqrt (std::max (lo, 20.0f) * std::min (hi, 20000.0f));
        }
    }

    // -----------------------------------------------------------------------
    void SpectralSuppressor::prepare (double sr, int /*maxBlockSize*/)
    {
        sampleRate = sr;
        vizInputBuf.assign (kVizSize, 0.0f);
        vizWrite.store (0);
        reset();
        agc.prepare (sr);
        update (sr);
    }

    void SpectralSuppressor::reset()
    {
        for (auto& c : suppCh) { c.split.reset(); for (auto& b : c.band) { b.env = 0.0f; b.gainDb = 0.0f; b.appliedDb = 999.0f; b.detect.reset(); b.cut.reset(); } }
        for (auto& c : ottCh)  { c.split.reset(); for (auto& b : c.band) { b.env.reset(); b.gainDb = 0.0f; } }
        for (auto& c : gateCh) { c.split.reset(); for (auto& b : c.band) { b.env.reset(); b.gainDb = 0.0f; } }
        for (auto& c : eqCh)   { for (auto& b : c.band) b.reset(); }
        agc.reset();

        vizWrite.store (0);
    }

    void SpectralSuppressor::setParams (const Params& np)
    {
        if (np == p)
            return;
        p = np;
        update (sampleRate);
    }

    // Maps the 0..5 `order` index to a permutation of the three modules.
    static void orderFromIndex (int idx, int out[3])
    {
        static const int table[6][3] = {
            { 0, 1, 2 },   // suppressor, OTT, gate
            { 0, 2, 1 },   // suppressor, gate, OTT
            { 1, 0, 2 },   // OTT, suppressor, gate
            { 1, 2, 0 },   // OTT, gate, suppressor
            { 2, 0, 1 },   // gate, suppressor, OTT
            { 2, 1, 0 },   // gate, OTT, suppressor
        };
        idx = std::clamp (idx, 0, 5);
        out[0] = table[idx][0];
        out[1] = table[idx][1];
        out[2] = table[idx][2];
    }

    void SpectralSuppressor::update (double sr)
    {
        const float fs = static_cast<float> (sr);

        orderFromIndex (p.order, orderArr);

        suppSlopeFactor = 0.5f + 1.5f * p.mod[0].suppSlope;

        for (int m = 0; m < kNumModules; ++m)
        {
            const auto& mp = p.mod[static_cast<size_t> (m)];
            const int nb   = std::clamp (mp.bands, 1, kMaxBands);
            const int nx   = nb - 1;

            // Band centre frequencies (for the peaking cut / visualization).
            for (int b = 0; b < nb; ++b)
                bandFreq[m][static_cast<size_t> (b)] = bandCenter (mp.xover, nb, b);

            // Configure splitters + per-band coefficients.
            for (auto& c : suppCh)
            {
                c.split.setCrossovers (fs, mp.xover, nx);
                for (int b = 0; b < nb; ++b)
                {
                    auto& sb = c.band[static_cast<size_t> (b)];
                    sb.freq = bandFreq[m][static_cast<size_t> (b)];
                    const float sens = mp.suppSensitivity[static_cast<size_t> (b)];
                    const float qDetect = 0.8f + 1.2f * (1.0f - sens);
                    const float qCut    = 1.0f + 5.0f * (1.0f - mp.suppQ[static_cast<size_t> (b)]);
                    sb.detect.setBandpass (fs, sb.freq, qDetect);
                    sb.cut.setPeaking (fs, sb.freq, qCut, sb.gainDb);
                    sb.appliedDb = sb.gainDb;
                }
            }

            for (auto& c : ottCh)
            {
                c.split.setCrossovers (fs, mp.xover, nx);
                for (int b = 0; b < nb; ++b)
                {
                    c.band[static_cast<size_t> (b)].env.setTimes (fs, mp.ottAttackMs, mp.ottReleaseMs);
                    c.band[static_cast<size_t> (b)].gainDb = 0.0f;
                }
            }

            for (auto& c : gateCh)
            {
                c.split.setCrossovers (fs, mp.xover, nx);
                for (int b = 0; b < nb; ++b)
                {
                    c.band[static_cast<size_t> (b)].env.setTimes (fs, mp.gateAttackMs, mp.gateReleaseMs);
                    c.band[static_cast<size_t> (b)].gainDb = 0.0f;
                }
            }
        }

        // Manual EQ (fixed Q = 1.0).
        for (int i = 0; i < kNumEqBands; ++i)
        {
            const float freq = std::clamp (p.eqFreq[i], 40.0f, fs * 0.45f);
            const float gain = std::clamp (p.eqGain[i], -18.0f, 18.0f);
            for (auto& c : eqCh)
                c.band[static_cast<size_t> (i)].setPeaking (fs, freq, 1.0f, gain);
        }
    }

    // -----------------------------------------------------------------------
    // Visualization accessors.
    // -----------------------------------------------------------------------
    bool SpectralSuppressor::isModuleEnabled (Module m) const
    {
        return p.mod[static_cast<int> (m)].enabled;
    }

    int SpectralSuppressor::getModuleBands (Module m) const
    {
        return std::clamp (p.mod[static_cast<int> (m)].bands, 1, kMaxBands);
    }

    float SpectralSuppressor::getModuleBandFreq (Module m, int band) const
    {
        const int nb = getModuleBands (m);
        if (band < 0 || band >= nb) return 0.0f;
        return bandFreq[static_cast<int> (m)][static_cast<size_t> (band)];
    }

    float SpectralSuppressor::getModuleBandGainDb (Module m, int band) const
    {
        const int nb = getModuleBands (m);
        if (band < 0 || band >= nb || !isModuleEnabled (m)) return 0.0f;

        switch (m)
        {
            case Module::Suppressor: return suppCh[0].band[static_cast<size_t> (band)].gainDb;
            case Module::Ott:        return ottCh[0].band[static_cast<size_t> (band)].gainDb;
            case Module::Gate:       return gateCh[0].band[static_cast<size_t> (band)].gainDb;
        }
        return 0.0f;
    }

    // -----------------------------------------------------------------------
    // Per-module mono processing (channel `ch`).
    // -----------------------------------------------------------------------
    void SpectralSuppressor::processModule (Module m, float& x, int ch)
    {
        switch (m)
        {
            case Module::Suppressor: x = suppressor (m, x, ch); break;
            case Module::Ott:        x = ott (m, x, ch); break;
            case Module::Gate:       x = gate (m, x, ch); break;
        }
    }

    float SpectralSuppressor::suppressor (Module /*m*/, float x, int ch)
    {
        const auto& mp = p.mod[0];
        const int nb   = std::clamp (mp.bands, 1, kMaxBands);
        const float fs = static_cast<float> (sampleRate);

        float bands[kMaxBands];
        suppCh[ch].split.process (x, bands, nb);

        float y = 0.0f;
        for (int b = 0; b < nb; ++b)
        {
            auto& sb = suppCh[ch].band[static_cast<size_t> (b)];
            const float detect = std::fabs (sb.detect.process (bands[b]));
            if (detect > sb.env) sb.env = detect;
            else
            {
                const float rel = 1.0f - std::exp (-1.0f / (std::max (0.1f, mp.suppReleaseMs) * 0.001f * fs));
                sb.env += rel * (detect - sb.env);
            }

            float target = 0.0f;
            const float sensitivity = mp.suppSensitivity[static_cast<size_t> (b)];
            const float thresholdLin = std::pow (10.0f, (-10.0f - sensitivity * 40.0f) / 20.0f);
            if (sb.env > thresholdLin)
            {
                const float overDb = 20.0f * std::log10 (sb.env / thresholdLin);
                target = std::clamp (-mp.suppAmount[static_cast<size_t> (b)] * overDb * suppSlopeFactor, -maxCutDb, 0.0f);
            }

            const float att = 1.0f - std::exp (-1.0f / (std::max (0.1f, mp.suppAttackMs) * 0.001f * fs));
            const float rel = 1.0f - std::exp (-1.0f / (std::max (0.1f, mp.suppReleaseMs) * 0.001f * fs));
            const float coef = (target < sb.gainDb) ? att : rel;
            sb.gainDb += coef * (target - sb.gainDb);

            if (std::fabs (sb.gainDb - sb.appliedDb) > 0.05f)
            {
                const float qCut = 1.0f + 5.0f * (1.0f - mp.suppQ[static_cast<size_t> (b)]);
                sb.cut.setPeaking (fs, sb.freq, qCut, sb.gainDb);
                sb.appliedDb = sb.gainDb;
            }

            y += sb.cut.process (bands[b]);
        }

        return y;
    }

    float SpectralSuppressor::ott (Module /*m*/, float x, int ch)
    {
        const auto& mp = p.mod[1];
        const int nb   = std::clamp (mp.bands, 1, kMaxBands);

        float bands[kMaxBands];
        ottCh[ch].split.process (x, bands, nb);

        float y = 0.0f;
        for (int b = 0; b < nb; ++b)
        {
            auto& ob = ottCh[ch].band[static_cast<size_t> (b)];
            const float env = ob.env.process (bands[b]);
            const float levelDb = 20.0f * std::log10 (std::max (env, 1.0e-5f));

            const float thDb = mp.ottThresholdDb[static_cast<size_t> (b)];

            // Downward compression above threshold (ratio ~4:1 -> slope 0.75).
            float targetDb = 0.0f;
            if (levelDb > thDb)
                targetDb = -(levelDb - thDb) * 0.75f * mp.ottDown[static_cast<size_t> (b)];

            // Upward compression below threshold (ratio ~2:1 -> slope 0.5).
            if (levelDb < thDb)
                targetDb = (thDb - levelDb) * 0.5f * mp.ottUp[static_cast<size_t> (b)];

            targetDb = std::clamp (targetDb, -24.0f, 24.0f);

            const float att = 1.0f - std::exp (-1.0f / (std::max (0.1f, mp.ottAttackMs) * 0.001f * static_cast<float> (sampleRate)));
            const float rel = 1.0f - std::exp (-1.0f / (std::max (0.1f, mp.ottReleaseMs) * 0.001f * static_cast<float> (sampleRate)));
            const float coef = (targetDb < ob.gainDb) ? att : rel;
            ob.gainDb += coef * (targetDb - ob.gainDb);

            y += bands[b] * std::pow (10.0f, ob.gainDb / 20.0f);
        }

        return y;
    }

    float SpectralSuppressor::gate (Module /*m*/, float x, int ch)
    {
        const auto& mp = p.mod[2];
        const int nb   = std::clamp (mp.bands, 1, kMaxBands);

        float bands[kMaxBands];
        gateCh[ch].split.process (x, bands, nb);

        float y = 0.0f;
        for (int b = 0; b < nb; ++b)
        {
            auto& gb = gateCh[ch].band[static_cast<size_t> (b)];
            const float env = gb.env.process (bands[b]);
            const float levelDb = 20.0f * std::log10 (std::max (env, 1.0e-5f));

            const float thDb    = mp.gateThresholdDb[static_cast<size_t> (b)];
            const float rangeDb = mp.gateRangeDb[static_cast<size_t> (b)];
            const float targetDb = (levelDb < thDb) ? rangeDb : 0.0f;

            const float att = 1.0f - std::exp (-1.0f / (std::max (0.1f, mp.gateAttackMs) * 0.001f * static_cast<float> (sampleRate)));
            const float rel = 1.0f - std::exp (-1.0f / (std::max (0.1f, mp.gateReleaseMs) * 0.001f * static_cast<float> (sampleRate)));
            const float coef = (targetDb < gb.gainDb) ? att : rel;
            gb.gainDb += coef * (targetDb - gb.gainDb);

            y += bands[b] * std::pow (10.0f, gb.gainDb / 20.0f);
        }

        return y;
    }

    // -----------------------------------------------------------------------
    void SpectralSuppressor::process (const float* inL, const float* inR,
                                      float* outL, float* outR, int numSamples)
    {
        const float mix    = p.mix;
        const float output = p.output;

        for (int s = 0; s < numSamples; ++s)
        {
            // Ring buffer of mono input for the spectrum display.
            const float mono = 0.5f * (inL[s] + inR[s]);
            const int w = vizWrite.load();
            vizInputBuf[static_cast<size_t> (w)] = mono;
            vizWrite.store ((w + 1) % kVizSize);

            float wet[2];
            for (int ch = 0; ch < 2; ++ch)
            {
                const float dry = (ch == 0) ? inL[s] : inR[s];

                float x = dry;
                for (int i = 0; i < kNumModules; ++i)
                {
                    const Module m = static_cast<Module> (orderArr[i]);
                    if (p.mod[static_cast<int> (m)].enabled)
                        processModule (m, x, ch);
                }

                // Manual EQ (in series, always last).
                for (int i = 0; i < kNumEqBands; ++i)
                    x = eqCh[ch].band[static_cast<size_t> (i)].process (x);

                wet[ch] = x;
            }

            // Optional gain compensation (off by default).
            float makeup = 1.0f;
            if (p.autoGain)
                makeup = agc.process (std::max (std::fabs (inL[s]), std::fabs (inR[s])),
                                      std::max (std::fabs (wet[0]), std::fabs (wet[1])));

            for (int ch = 0; ch < 2; ++ch)
            {
                const float dry = (ch == 0) ? inL[s] : inR[s];
                const float out = std::clamp ((dry * (1.0f - mix) + wet[ch] * makeup * mix) * output, -1.0f, 1.0f);
                if (ch == 0) outL[s] = out; else outR[s] = out;
            }
        }
    }

} // namespace temper
