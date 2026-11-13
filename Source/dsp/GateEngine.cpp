#include "GateEngine.h"

#include <algorithm>
#include <cmath>

namespace temper
{
    namespace
    {
        inline float dbToLin (float db) { return std::exp (db * 0.11512925465f); }
        int specSizeFor (double sr)
        {
            int scale = 1;
            while (sr / (48000.0 * scale) > 1.5) scale *= 2;
            return 2048 * scale;
        }
    }

    int GateEngine::maxLatency (double sampleRate)
    {
        return std::max (specSizeFor (sampleRate), static_cast<int> (0.01 * sampleRate) + 1);
    }

    int GateEngine::latency() const { return st.engine == 1 ? specN : lookSamples; }

    void GateEngine::prepare (double sampleRate)
    {
        sr = sampleRate;
        specN = specSizeFor (sr);
        stft.prepare (specN);
        stft.configure (specN);
        const int maxLook = static_cast<int> (0.01 * sr) + 1;
        for (auto& ch : look) for (auto& d : ch) d.prepare (maxLook);
        offL.prepare (maxLatency (sr)); offR.prepare (maxLatency (sr));
        const size_t B = static_cast<size_t> (specN / 2 + 1);
        binFreq.assign (B, 0.0f);
        for (size_t k = 0; k < B; ++k) binFreq[k] = static_cast<float> (k * sr / specN);
        scWeight.assign (B, 1.0f); profileBin.assign (B, 0.0f); level.assign (B, -200.0f);
        gSpec.assign (B, 0.0f); gTmp.assign (B, 0.0f); holdF.assign (B, 0.0f); openBin.assign (B, 0);
        for (auto& m : mask) m.assign (B, 0.0f);
        learnSum.assign (B, 0.0);
        prepared = true;
        const GateSettings s = st;
        st.x1 = -1.0f;   // force a full update
        setSettings (s);
        reset();
    }

    void GateEngine::reset()
    {
        for (auto& s : split) s.reset();
        for (auto& s : detSplit) s.reset();
        for (auto& ch : scFilt) for (auto& f : ch) f.reset();
        for (auto& ch : look) for (auto& d : ch) d.reset();
        env.fill (0.0f); gDb.fill (st.flip ? 0.0f : -60.0f); open.fill (false); hold.fill (0);
        offL.reset(); offR.reset();
        stft.reset();
        std::fill (gSpec.begin(), gSpec.end(), 0.0f);
        std::fill (openBin.begin(), openBin.end(), static_cast<unsigned char> (0));
        std::fill (holdF.begin(), holdF.end(), 0.0f);
    }

    void GateEngine::setSettings (const GateSettings& s)
    {
        if (! prepared) { st = s; return; }
        if (s == st) return;
        const bool engineChanged = s.engine != st.engine;
        const bool masksChanged = s.x1 != st.x1 || s.x2 != st.x2 || s.bands != st.bands || s.scHpf != st.scHpf || s.scLpf != st.scLpf;
        st = s;
        for (auto& sp : split)    sp.set (sr, st.x1, st.x2, st.bands);
        for (auto& sp : detSplit) sp.set (sr, st.x1, st.x2, st.bands);
        FilterNode hp; hp.on = true; hp.type = static_cast<int> (FilterType::LowCut);  hp.freq = st.scHpf; hp.q = 0.7071f;
        FilterNode lp; lp.on = true; lp.type = static_cast<int> (FilterType::HighCut); lp.freq = st.scLpf; lp.q = 0.7071f;
        std::array<BiquadCoefs, 4> c;
        makeNodeCoefs (hp, sr, c); for (auto& ch : scFilt) ch[0].set (st.scHpf > 20.5f ? c[0] : BiquadCoefs());
        makeNodeCoefs (lp, sr, c); for (auto& ch : scFilt) ch[1].set (st.scLpf < 19900.0f ? c[0] : BiquadCoefs());
        attC = 1.0f - std::exp (-1.0f / (std::max (0.02f, st.attMs) * 0.001f * static_cast<float> (sr)));
        relC = 1.0f - std::exp (-1.0f / (std::max (1.0f, st.relMs) * 0.001f * static_cast<float> (sr)));
        envDecay = std::exp (-1.0f / static_cast<float> (0.005 * sr));
        holdSamples = static_cast<int> (st.holdMs * 0.001 * sr);
        lookSamples = std::clamp (static_cast<int> (std::lround (st.lookMs * 0.001 * sr)), 0, static_cast<int> (0.01 * sr));
        if (masksChanged) rebuildMasks();
        if (engineChanged) reset();
    }

    void GateEngine::rebuildMasks()
    {
        for (int b = 0; b < kDynBands; ++b)
        {
            double sum = 0.0;
            for (size_t k = 0; k < binFreq.size(); ++k)
            {
                const float w = (b < st.bands) ? bandMask (st.bands, st.x1, st.x2, std::max (binFreq[k], 1.0f), b) : 0.0f;
                mask[static_cast<size_t> (b)][k] = w;
                sum += w;
            }
            maskBins[static_cast<size_t> (b)] = static_cast<float> (std::max (sum, 1.0));
        }
        FilterNode hp; hp.on = true; hp.type = static_cast<int> (FilterType::LowCut);  hp.freq = st.scHpf; hp.q = 0.7071f;
        FilterNode lp; lp.on = true; lp.type = static_cast<int> (FilterType::HighCut); lp.freq = st.scLpf; lp.q = 0.7071f;
        // At their end stops (20 Hz / 20 kHz) the filters are off.
        const bool useHp = st.scHpf > 20.5f, useLp = st.scLpf < 19900.0f;
        for (size_t k = 0; k < binFreq.size(); ++k)
        {
            const float f = std::max (binFreq[k], 1.0f);
            const float db = (useHp ? nodeResponseDb (hp, sr, f) : 0.0f) + (useLp ? nodeResponseDb (lp, sr, f) : 0.0f);
            scWeight[k] = dbToLin (2.0f * db);   // power
        }
    }

    // Closed: below the close threshold the gain falls with (ratio - 1) dB per dB, down to range.
    float GateEngine::targetGain (const GateBand& b, float levelDb, bool isOpen) const
    {
        float g = 0.0f;
        if (! isOpen)
        {
            const float close = b.thrDb - st.hystDb;
            g = (b.ratio >= 50.0f) ? b.rangeDb : std::max (b.rangeDb, std::min (0.0f, (levelDb - close) * (b.ratio - 1.0f)));
        }
        return st.flip ? b.rangeDb - g : g;   // flip: duck while open
    }

    void GateEngine::process (float& l, float& r, float scl, float scr)
    {
        if (! st.enabled)
        {
            const int lat = latency();
            l = offL.process (l, lat); r = offR.process (r, lat);
            return;
        }
        if (st.engine == 0) { processBand (l, r, scl, scr); return; }
        stft.process (l, r, scl, scr, st.sidechain, [this] (const StereoStft& s, std::array<std::vector<float>, 2>& g) { spectralFrame (s, g); });
    }

    void GateEngine::processBand (float& l, float& r, float scl, float scr)
    {
        const int nb = std::clamp (st.bands, 1, kDynBands);
        float audio[2][kDynBands], det[2][kDynBands];
        const float x[2] = { l, r };
        const float d[2] = { st.sidechain ? scl : l, st.sidechain ? scr : r };
        for (int c = 0; c < 2; ++c)
        {
            split[static_cast<size_t> (c)].process (x[c], audio[c]);
            const float f = scFilt[static_cast<size_t> (c)][1].process (scFilt[static_cast<size_t> (c)][0].process (d[c]));
            detSplit[static_cast<size_t> (c)].process (f, det[c]);
        }
        float out[2] = { 0.0f, 0.0f };
        for (int b = 0; b < nb; ++b)
        {
            const auto& bp = st.band[static_cast<size_t> (b)];
            float& e = env[static_cast<size_t> (b)];
            e = std::max (std::max (std::fabs (det[0][b]), std::fabs (det[1][b])), e * envDecay);   // linked
            const float lv = 20.0f * std::log10 (e + 1.0e-12f);
            bool& o = open[static_cast<size_t> (b)];
            int& h = hold[static_cast<size_t> (b)];
            if (lv >= bp.thrDb) { o = true; h = holdSamples; }
            else if (lv < bp.thrDb - st.hystDb) { if (h > 0) --h; else o = false; }
            const float t = targetGain (bp, lv, o);
            float& g = gDb[static_cast<size_t> (b)];
            g += ((t > g) ? attC : relC) * (t - g);          // opening = attack
            const float gl = dbToLin (g);
            for (int c = 0; c < 2; ++c)
                out[c] += look[static_cast<size_t> (c)][static_cast<size_t> (b)].process (audio[c][b], lookSamples) * gl;
            gainViz[static_cast<size_t> (b)].store (g, std::memory_order_relaxed);
        }
        l = out[0]; r = out[1];
        if (++vizCount >= 256)
        {
            vizCount = 0;
            for (int p = 0; p < kCurvePoints; ++p)
            {
                float g = 0.0f;
                for (int b = 0; b < nb; ++b) g += bandMask (nb, st.x1, st.x2, curvePointFreq (p), b) * gainViz[static_cast<size_t> (b)].load (std::memory_order_relaxed);
                viz[static_cast<size_t> (p)].store (g, std::memory_order_relaxed);
            }
        }
    }

    void GateEngine::getProfile (float* dB) const { for (int i = 0; i < kCurvePoints; ++i) dB[i] = profile[static_cast<size_t> (i)]; }
    void GateEngine::setProfile (const float* dB) { for (int i = 0; i < kCurvePoints; ++i) pendingProfile[static_cast<size_t> (i)] = dB[i]; pendingSet.store (true); }

    void GateEngine::spectralFrame (const StereoStft& s, std::array<std::vector<float>, 2>& gain)
    {
        const int H = s.bins() - 1;
        const float hopSec = static_cast<float> (s.hopSize() / sr);
        const auto rebuildProfileBins = [&]
        {
            for (int k = 0; k <= H; ++k)
            {
                const float f = std::max (binFreq[static_cast<size_t> (k)], 20.0f);
                const float x = std::log (f / 20.0f) / std::log (1000.0f) * static_cast<float> (kCurvePoints - 1);
                const int i0 = std::clamp (static_cast<int> (x), 0, kCurvePoints - 1), i1 = std::min (i0 + 1, kCurvePoints - 1);
                const float t = std::clamp (x - static_cast<float> (i0), 0.0f, 1.0f);
                profileBin[static_cast<size_t> (k)] = profile[static_cast<size_t> (i0)] * (1.0f - t) + profile[static_cast<size_t> (i1)] * t;
            }
        };

        // Learn requests: average power per bin (both channels), stored as dB on display points.
        if (const int req = learnRequest.exchange (0))
        {
            if (req == 1) { std::fill (learnSum.begin(), learnSum.end(), 0.0); learnFrames = 0; learning.store (true); }
            else if (req == 2 && learning.load())
            {
                learning.store (false);
                if (learnFrames > 0)
                {
                    for (int p = 0; p < kCurvePoints; ++p)
                    {
                        const float f = curvePointFreq (p);
                        const int lo = std::clamp (static_cast<int> (f * std::pow (2.0f, -1.0f / 12.0f) * specN / sr), 1, H);
                        const int hi = std::clamp (static_cast<int> (f * std::pow (2.0f, 1.0f / 12.0f) * specN / sr), lo, H);
                        double sum = 0.0;
                        for (int k = lo; k <= hi; ++k) sum += learnSum[static_cast<size_t> (k)];
                        profile[static_cast<size_t> (p)] = static_cast<float> (10.0 * std::log10 (sum / ((hi - lo + 1) * static_cast<double> (learnFrames)) + 1e-24));
                    }
                    profileValid.store (true);
                    rebuildProfileBins();
                }
            }
        }
        if (pendingSet.exchange (false)) { profile = pendingProfile; profileValid.store (true); rebuildProfileBins(); }
        if (clearRequest.exchange (false)) profileValid.store (false);

        const auto& P0 = st.sidechain ? s.scPow[0] : s.pow[0];
        const auto& P1 = st.sidechain ? s.scPow[1] : s.pow[1];
        if (learning.load())
        {
            for (int k = 0; k <= H; ++k) learnSum[static_cast<size_t> (k)] += 0.5 * (P0[static_cast<size_t> (k)] + P1[static_cast<size_t> (k)]);
            ++learnFrames;
        }

        const bool prof = st.useProfile && profileValid.load();
        const float fa = 1.0f - std::exp (-hopSec / std::max (0.0005f, st.attMs * 0.001f));
        const float fr = 1.0f - std::exp (-hopSec / std::max (0.001f, st.relMs * 0.001f));
        const float holdFrames = st.holdMs * 0.001f / hopSec;
        for (int k = 0; k <= H; ++k)
        {
            // Linked power, smoothed over 3 bins and blended with the last frame. Here the
            // sidechain filters choose which frequencies are gated at all: a bin they cut by more
            // than 6 dB is left alone (a per-bin gate can't be "triggered" by other bins).
            const auto pk = [&] (int i) { i = std::clamp (i, 0, H); return std::max (P0[static_cast<size_t> (i)], P1[static_cast<size_t> (i)]); };
            const float p = 0.25f * pk (k - 1) + 0.5f * pk (k) + 0.25f * pk (k + 1);
            float& lv = level[static_cast<size_t> (k)];
            float bandScale = 0.0f, thr = 0.0f;
            GateBand gb { 0.0f, 0.0f, 0.0f };
            for (int b = 0; b < st.bands; ++b)
            {
                const float w = mask[static_cast<size_t> (b)][static_cast<size_t> (k)];
                bandScale += w * maskBins[static_cast<size_t> (b)];
                gb.thrDb += w * st.band[static_cast<size_t> (b)].thrDb;
                gb.rangeDb += w * st.band[static_cast<size_t> (b)].rangeDb;
                gb.ratio += w * st.band[static_cast<size_t> (b)].ratio;
            }
            // Fixed: the bin's level as its band would read if all of it looked like this bin.
            // With a profile: the bin's own level against the noise there.
            const float now = prof ? 10.0f * std::log10 (p + 1e-24f) : 10.0f * std::log10 (p * bandScale / 3.0f + 1e-24f);
            lv = (lv < -150.0f) ? now : 0.5f * (lv + now);
            thr = gb.thrDb + (prof ? profileBin[static_cast<size_t> (k)] : 0.0f);
            gb.thrDb = thr;
            unsigned char& o = openBin[static_cast<size_t> (k)];
            float& h = holdF[static_cast<size_t> (k)];
            if (lv >= thr) { o = 1; h = holdFrames; }
            else if (lv < thr - st.hystDb) { if (h > 0.0f) h -= 1.0f; else o = 0; }
            const float t = (scWeight[static_cast<size_t> (k)] < 0.25f) ? 0.0f : targetGain (gb, lv, o != 0);
            float& g = gSpec[static_cast<size_t> (k)];
            g += ((t > g) ? fa : fr) * (t - g);
        }
        // Smoothing across frequency (a box of up to +-6 bins) against isolated flickering bins.
        const int m = static_cast<int> (std::lround (std::clamp (st.smoothing, 0.0f, 1.0f) * 6.0f));
        if (m > 0)
        {
            double acc = 0.0;
            int lo = 0, hi = -1;
            for (int k = 0; k <= H; ++k)
            {
                const int nlo = std::max (0, k - m), nhi = std::min (H, k + m);
                while (hi < nhi) acc += gSpec[static_cast<size_t> (++hi)];
                while (lo < nlo) acc -= gSpec[static_cast<size_t> (lo++)];
                gTmp[static_cast<size_t> (k)] = static_cast<float> (acc / (hi - lo + 1));
            }
        }
        else
            gTmp = gSpec;
        for (int k = 0; k <= H; ++k)
        {
            const float gl = dbToLin (gTmp[static_cast<size_t> (k)]);
            gain[0][static_cast<size_t> (k)] = gl; gain[1][static_cast<size_t> (k)] = gl;
        }
        for (int p = 0; p < kCurvePoints; ++p)
        {
            const int k = std::clamp (static_cast<int> (std::lround (curvePointFreq (p) * specN / sr)), 0, H);
            viz[static_cast<size_t> (p)].store (gTmp[static_cast<size_t> (k)], std::memory_order_relaxed);
        }
        for (int b = 0; b < kDynBands; ++b)
        {
            double s2 = 0.0;
            for (int k = 0; k <= H; ++k) s2 += mask[static_cast<size_t> (b)][static_cast<size_t> (k)] * gTmp[static_cast<size_t> (k)];
            gainViz[static_cast<size_t> (b)].store (static_cast<float> (s2 / maskBins[static_cast<size_t> (b)]), std::memory_order_relaxed);
        }
    }
} // namespace temper
