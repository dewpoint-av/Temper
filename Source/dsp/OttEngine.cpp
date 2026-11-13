#include "OttEngine.h"

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
            return 1024 * scale;
        }
        float softOver (float over, float knee)
        {
            if (knee <= 0.0f) return std::max (over, 0.0f);
            if (over <= -0.5f * knee) return 0.0f;
            if (over < 0.5f * knee) { const float t = over + 0.5f * knee; return t * t / (2.0f * knee); }
            return over;
        }
    }

    int OttEngine::maxLatency (double sampleRate)
    {
        return std::max (specSizeFor (sampleRate), static_cast<int> (0.01 * sampleRate) + 1);
    }

    int OttEngine::latency() const
    {
        return st.engine == 1 ? specN : lookSamples;
    }

    void OttEngine::prepare (double sampleRate)
    {
        sr = sampleRate;
        specN = specSizeFor (sr);
        stft.prepare (specN);
        stft.configure (specN);
        const int maxLook = static_cast<int> (0.01 * sr) + 1;
        for (auto& ch : look) for (auto& d : ch) d.prepare (maxLook);
        offDelayL.prepare (maxLatency (sr));
        offDelayR.prepare (maxLatency (sr));
        const size_t B = static_cast<size_t> (specN / 2 + 1);
        binFreq.assign (B, 0.0f);
        for (size_t k = 0; k < B; ++k) binFreq[k] = static_cast<float> (k * sr / specN);
        for (auto& m : mask) m.assign (B, 0.0f);
        for (int c = 0; c < 2; ++c) { specG[c].assign (B, 0.0f); specG2[c].assign (B, 0.0f); levelBuf[c].assign (B, 0.0f); }
        prefix.assign (B + 1, 0.0);
        halfW.assign (B, 0);
        prepared = true;
        const OttSettings s = st;
        st.x1 = -1.0f;            // force a full update
        setSettings (s);
        reset();
    }

    void OttEngine::reset()
    {
        for (auto& s : split) s.reset();
        for (auto& s : scSplit) s.reset();
        for (auto& ch : look) for (auto& d : ch) d.reset();
        for (int c = 0; c < 2; ++c) { ms[c].fill (0.0f); gDb[c].fill (0.0f); gDb2[c].fill (0.0f); }
        offDelayL.reset(); offDelayR.reset();
        stft.reset();
        for (int c = 0; c < 2; ++c) { std::fill (specG[c].begin(), specG[c].end(), 0.0f); std::fill (specG2[c].begin(), specG2[c].end(), 0.0f); }
    }

    void OttEngine::setSettings (const OttSettings& s)
    {
        if (! prepared) { st = s; return; }
        if (s == st) return;
        const bool engineChanged = s.engine != st.engine;
        const bool bandsChanged = s.x1 != st.x1 || s.x2 != st.x2;
        const bool resChanged = s.resolution != st.resolution || bandsChanged;
        st = s;

        for (auto& sp : split)   sp.set (sr, st.x1, st.x2, kDynBands);
        for (auto& sp : scSplit) sp.set (sr, st.x1, st.x2, kDynBands);
        lookSamples = std::clamp (static_cast<int> (std::lround (st.lookaheadMs * 0.001 * sr)), 0, static_cast<int> (0.01 * sr));
        rmsCoef = 1.0f - std::exp (-1.0f / static_cast<float> ((st.style == 1 ? 0.015 : 0.005) * sr));

        // Timing styles: Classic = the times as set; Smooth = two half-time stages in series;
        // Smack = slower attack (transients punch through), quicker release.
        const float attMul = st.style == 2 ? 2.5f : (st.style == 1 ? 0.5f : 1.0f);
        const float relMul = st.style == 2 ? 0.6f : (st.style == 1 ? 0.5f : 1.0f);
        for (int b = 0; b < kDynBands; ++b)
        {
            const float a = std::max (0.05f, st.band[static_cast<size_t> (b)].attMs * st.time * attMul);
            const float r = std::max (1.0f, st.band[static_cast<size_t> (b)].relMs * st.time * relMul);
            attC[static_cast<size_t> (b)] = 1.0f - std::exp (-1.0f / (a * 0.001f * static_cast<float> (sr)));
            relC[static_cast<size_t> (b)] = 1.0f - std::exp (-1.0f / (r * 0.001f * static_cast<float> (sr)));
        }

        if (bandsChanged)
        {
            for (int b = 0; b < kDynBands; ++b)
            {
                double sum = 0.0;
                for (size_t k = 0; k < binFreq.size(); ++k)
                {
                    mask[static_cast<size_t> (b)][k] = bandMask (kDynBands, st.x1, st.x2, std::max (binFreq[k], 1.0f), b);
                    sum += mask[static_cast<size_t> (b)][k];
                }
                maskBins[static_cast<size_t> (b)] = static_cast<float> (std::max (sum, 1.0));
            }
        }
        if (resChanged)
        {
            const float wOct = std::max ((1.0f - std::clamp (st.resolution, 0.0f, 1.0f)) * 1.0f, 0.05f);
            const float c = std::pow (2.0f, 0.5f * wOct) - 1.0f;
            for (size_t k = 0; k < halfW.size(); ++k) halfW[k] = static_cast<int> (std::floor (static_cast<float> (k) * c));
        }
        if (engineChanged) reset();
    }

    float OttEngine::computeGain (const OttBand& b, float levelDb, float amt) const
    {
        const float knee = st.softKnee ? 6.0f : 0.0f;
        const float gd = -b.down * amt * softOver (levelDb - b.aboveDb, knee);
        const float gu = std::min (b.up * amt * softOver (b.belowDb - levelDb, knee), 36.0f);
        return gd + gu;
    }

    void OttEngine::process (float& l, float& r, float scl, float scr)
    {
        if (! st.enabled)
        {
            // Off: the same latency as on, so switching it never moves the timing.
            const int lat = latency();
            l = offDelayL.process (l, lat);
            r = offDelayR.process (r, lat);
            return;
        }
        if (st.engine == 0) { processMultiband (l, r, scl, scr); return; }

        stft.process (l, r, scl, scr, st.sidechain, [this] (const StereoStft& s, std::array<std::vector<float>, 2>& g) { spectralFrame (s, g); });
        if (st.clip)
        {
            const float c = dbToLin (st.clipDb);
            l = std::clamp (l, -c, c); r = std::clamp (r, -c, c);
        }
    }

    void OttEngine::processMultiband (float& l, float& r, float scl, float scr)
    {
        const float gin = dbToLin (st.inDb), gout = dbToLin (st.outDb);
        float bands[2][kDynBands], det[2][kDynBands];
        const float x[2] = { l * gin, r * gin };
        for (int c = 0; c < 2; ++c) split[static_cast<size_t> (c)].process (x[c], bands[c]);
        if (st.sidechain)
        {
            const float s[2] = { scl * gin, scr * gin };
            for (int c = 0; c < 2; ++c)
            {
                if (st.scWide) for (int b = 0; b < kDynBands; ++b) det[c][b] = s[c];
                else scSplit[static_cast<size_t> (c)].process (s[c], det[c]);
            }
        }
        else
            for (int c = 0; c < 2; ++c) for (int b = 0; b < kDynBands; ++b) det[c][b] = bands[c][b];

        const float peakDecay = std::exp (-1.0f / static_cast<float> (0.01 * sr));
        bool anySolo = false;
        for (const auto& b : st.band) anySolo = anySolo || b.solo;
        float out[2] = { 0.0f, 0.0f };
        const float clipLin = dbToLin (st.clipDb);

        for (int b = 0; b < kDynBands; ++b)
        {
            const auto& bp = st.band[static_cast<size_t> (b)];
            const float amt = st.amount * bp.amount;
            const float bin = dbToLin (bp.inDb * amt);
            float lv[2];
            for (int c = 0; c < 2; ++c)
            {
                const float v = det[c][b] * bin;
                float& m = ms[static_cast<size_t> (c)][static_cast<size_t> (b)];
                if (st.rms) m += rmsCoef * (v * v - m);
                else        m = std::max (v * v, m * peakDecay * peakDecay);
                lv[c] = 10.0f * std::log10 (m + 1.0e-20f);
            }
            // Stereo link: 0 % own level, 100 % the louder, 200 % the other channel's.
            const float hi = std::max (lv[0], lv[1]);
            float ll[2];
            for (int c = 0; c < 2; ++c)
                ll[c] = st.link <= 1.0f ? lv[c] + (hi - lv[c]) * st.link : hi + (lv[1 - c] - hi) * (st.link - 1.0f);

            const float tone = (b == 0 ? st.lowTone : (b == kDynBands - 1 ? st.highTone : 0.0f));
            for (int c = 0; c < 2; ++c)
            {
                const float t = computeGain (bp, ll[c], amt);
                float& g = gDb[static_cast<size_t> (c)][static_cast<size_t> (b)];
                g += ((t < g) ? attC[static_cast<size_t> (b)] : relC[static_cast<size_t> (b)]) * (t - g);
                float ge = g;
                if (st.style == 1)
                {
                    float& g2 = gDb2[static_cast<size_t> (c)][static_cast<size_t> (b)];
                    g2 += ((g < g2) ? attC[static_cast<size_t> (b)] : relC[static_cast<size_t> (b)]) * (g - g2);
                    ge = g2;
                }
                const float y = look[static_cast<size_t> (c)][static_cast<size_t> (b)].process (bands[c][b], lookSamples);
                float v;
                if (bp.bypass) v = y;
                else
                {
                    v = y * dbToLin (ge + (bp.inDb + bp.outDb) * amt + tone);
                    if (st.clip) v = std::clamp (v, -clipLin, clipLin);
                }
                if (! anySolo || bp.solo) out[c] += v;
                if (c == 0) { levelViz[static_cast<size_t> (b)].store (ll[0], std::memory_order_relaxed); gainViz[static_cast<size_t> (b)].store (bp.bypass ? 0.0f : ge, std::memory_order_relaxed); }
            }
        }
        l = out[0] * gout;
        r = out[1] * gout;
        updateViz();
    }

    void OttEngine::spectralFrame (const StereoStft& s, std::array<std::vector<float>, 2>& gain)
    {
        const int H = s.bins() - 1;
        const float hopSec = static_cast<float> (s.hopSize() / sr);
        const float res = std::clamp (st.resolution, 0.0f, 1.0f);
        bool anySolo = false;
        for (const auto& b : st.band) anySolo = anySolo || b.solo;

        // Per band: frame-rate attack / release (Smack / Smooth as in the multiband engine).
        const float attMul = st.style == 2 ? 2.5f : (st.style == 1 ? 0.5f : 1.0f);
        const float relMul = st.style == 2 ? 0.6f : (st.style == 1 ? 0.5f : 1.0f);
        std::array<float, kDynBands> fa {}, fr {}, amt {}, inLin2 {};
        for (int b = 0; b < kDynBands; ++b)
        {
            const auto& bp = st.band[static_cast<size_t> (b)];
            fa[static_cast<size_t> (b)] = 1.0f - std::exp (-hopSec / std::max (0.0005f, bp.attMs * st.time * attMul * 0.001f));
            fr[static_cast<size_t> (b)] = 1.0f - std::exp (-hopSec / std::max (0.001f, bp.relMs * st.time * relMul * 0.001f));
            amt[static_cast<size_t> (b)] = st.amount * bp.amount;
            const float g = dbToLin (st.inDb + bp.inDb * amt[static_cast<size_t> (b)]);
            inLin2[static_cast<size_t> (b)] = g * g;
        }

        std::array<std::array<float, kDynBands>, 2> bandDb {};

        for (int c = 0; c < 2; ++c)
        {
            const std::vector<float>& P = st.sidechain ? s.scPow[static_cast<size_t> (c)] : s.pow[static_cast<size_t> (c)];
            // Band levels (RMS-equivalent: the Hann window spreads a sine over ~1.5 bins and a
            // sine's RMS is half its peak power, hence / 3).
            double wide = 0.0;
            for (int b = 0; b < kDynBands; ++b)
            {
                double sum = 0.0;
                for (int k = 1; k <= H; ++k) sum += static_cast<double> (mask[static_cast<size_t> (b)][static_cast<size_t> (k)]) * P[static_cast<size_t> (k)];
                wide += sum * inLin2[static_cast<size_t> (b)];
                bandDb[static_cast<size_t> (c)][static_cast<size_t> (b)] = static_cast<float> (10.0 * std::log10 (sum * inLin2[static_cast<size_t> (b)] / 3.0 + 1e-20));
            }
            if (st.sidechain && st.scWide)
                for (auto& v : bandDb[static_cast<size_t> (c)]) v = static_cast<float> (10.0 * std::log10 (wide / 3.0 + 1e-20));

            // Each bin's own neighbourhood, scaled to what its band would read if the whole band looked like it.
            float* L = levelBuf[c].data();
            if (res > 0.0f && ! (st.sidechain && st.scWide))
            {
                prefix[0] = 0.0;
                for (int k = 0; k <= H; ++k)
                {
                    float inl = 0.0f;
                    for (int b = 0; b < kDynBands; ++b) inl += mask[static_cast<size_t> (b)][static_cast<size_t> (k)] * inLin2[static_cast<size_t> (b)];
                    prefix[static_cast<size_t> (k + 1)] = prefix[static_cast<size_t> (k)] + P[static_cast<size_t> (k)] * inl;
                }
                for (int k = 0; k <= H; ++k)
                {
                    const int h = std::max (halfW[static_cast<size_t> (k)], 1);
                    const int lo = std::max (1, k - h), hi = std::min (H, k + h);
                    const double dens = (hi >= lo) ? (prefix[static_cast<size_t> (hi + 1)] - prefix[static_cast<size_t> (lo)]) / (hi - lo + 1) : 0.0;
                    float nb = 0.0f, bd = 0.0f;
                    for (int b = 0; b < kDynBands; ++b)
                    {
                        const float w = mask[static_cast<size_t> (b)][static_cast<size_t> (k)];
                        nb += w * maskBins[static_cast<size_t> (b)];
                        bd += w * bandDb[static_cast<size_t> (c)][static_cast<size_t> (b)];
                    }
                    const float local = static_cast<float> (10.0 * std::log10 (dens * nb / 3.0 + 1e-20));
                    L[k] = bd + (local - bd) * res;
                }
            }
            else
                for (int k = 0; k <= H; ++k)
                {
                    float bd = 0.0f;
                    for (int b = 0; b < kDynBands; ++b) bd += mask[static_cast<size_t> (b)][static_cast<size_t> (k)] * bandDb[static_cast<size_t> (c)][static_cast<size_t> (b)];
                    L[k] = bd;
                }
        }

        const float gIn = st.inDb, gOut = st.outDb;
        for (int k = 0; k <= H; ++k)
        {
            const float hi = std::max (levelBuf[0][static_cast<size_t> (k)], levelBuf[1][static_cast<size_t> (k)]);
            float lk[2];
            for (int c = 0; c < 2; ++c)
            {
                const float own = levelBuf[c][static_cast<size_t> (k)], other = levelBuf[1 - c][static_cast<size_t> (k)];
                lk[c] = st.link <= 1.0f ? own + (hi - own) * st.link : hi + (other - hi) * (st.link - 1.0f);
            }
            for (int c = 0; c < 2; ++c)
            {
                float t = 0.0f, a = 0.0f, rr = 0.0f;
                for (int b = 0; b < kDynBands; ++b)
                {
                    const float w = mask[static_cast<size_t> (b)][static_cast<size_t> (k)];
                    if (w <= 0.0f) continue;
                    t += w * computeGain (st.band[static_cast<size_t> (b)], lk[c], amt[static_cast<size_t> (b)]);
                    a += w * fa[static_cast<size_t> (b)]; rr += w * fr[static_cast<size_t> (b)];
                }
                float& g1 = specG[static_cast<size_t> (c)][static_cast<size_t> (k)];
                g1 += ((t < g1) ? a : rr) * (t - g1);
                float g = g1;
                if (st.style == 1)   // Smooth: a second stage
                {
                    float& g2 = specG2[static_cast<size_t> (c)][static_cast<size_t> (k)];
                    g2 += ((g1 < g2) ? a : rr) * (g1 - g2);
                    g = g2;
                }
                float lin = 0.0f;
                for (int b = 0; b < kDynBands; ++b)
                {
                    const float w = mask[static_cast<size_t> (b)][static_cast<size_t> (k)];
                    if (w <= 0.0f) continue;
                    const auto& bp = st.band[static_cast<size_t> (b)];
                    if (anySolo && ! bp.solo) continue;
                    const float tone = (b == 0 ? st.lowTone : (b == kDynBands - 1 ? st.highTone : 0.0f));
                    lin += w * (bp.bypass ? 1.0f : dbToLin (g + gIn + gOut + (bp.inDb + bp.outDb) * amt[static_cast<size_t> (b)] + tone));
                }
                gain[static_cast<size_t> (c)][static_cast<size_t> (k)] = lin;
            }
        }
        for (int b = 0; b < kDynBands; ++b)
        {
            levelViz[static_cast<size_t> (b)].store (bandDb[0][static_cast<size_t> (b)], std::memory_order_relaxed);
            // The band's typical gain: its mask-weighted mean.
            double s2 = 0.0;
            for (int k = 0; k <= H; ++k) s2 += mask[static_cast<size_t> (b)][static_cast<size_t> (k)] * specG[0][static_cast<size_t> (k)];
            gainViz[static_cast<size_t> (b)].store (static_cast<float> (s2 / maskBins[static_cast<size_t> (b)]), std::memory_order_relaxed);
        }
        // Per display point: the gain at its bin.
        for (int p = 0; p < kCurvePoints; ++p)
        {
            const int k = std::clamp (static_cast<int> (std::lround (curvePointFreq (p) * specN / sr)), 0, H);
            viz[static_cast<size_t> (p)].store (specG[0][static_cast<size_t> (k)], std::memory_order_relaxed);
        }
    }

    void OttEngine::updateViz()
    {
        // Multiband: the band gains spread over frequency by the same masks the spectral engine uses.
        if (++vizCount < 256) return;
        vizCount = 0;
        for (int p = 0; p < kCurvePoints; ++p)
        {
            const float f = curvePointFreq (p);
            float g = 0.0f;
            for (int b = 0; b < kDynBands; ++b) g += bandMask (kDynBands, st.x1, st.x2, f, b) * gainViz[static_cast<size_t> (b)].load (std::memory_order_relaxed);
            viz[static_cast<size_t> (p)].store (g, std::memory_order_relaxed);
        }
    }
} // namespace temper
