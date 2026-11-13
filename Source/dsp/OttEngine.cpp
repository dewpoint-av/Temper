#include "OttEngine.h"
#include "FastMath.h"

#include <algorithm>
#include <cmath>

namespace temper
{
    namespace
    {
        inline float dbToLin (float db) { return std::exp (db * 0.11512925465f); }
        int srScale (double sr)
        {
            int scale = 1;
            while (sr / (48000.0 * scale) > 1.5) scale *= 2;
            return scale;
        }
        int shortSizeFor (double sr) { return 1024 * srScale (sr); }
        int longSizeFor (double sr)  { return 4096 * srScale (sr); }
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
        return std::max (longSizeFor (sampleRate), static_cast<int> (0.01 * sampleRate) + 1);
    }

    int OttEngine::latency() const
    {
        return st.engine == 1 ? res[0].n : lookSamples;
    }

    void OttEngine::prepare (double sampleRate)
    {
        sr = sampleRate;
        const int maxLook = static_cast<int> (0.01 * sr) + 1;
        for (auto& ch : look) for (auto& d : ch) d.prepare (maxLook);
        for (auto& ch : lookSmooth) for (auto& m : ch) m.prepare (maxLook);
        offDelayL.prepare (maxLatency (sr));
        offDelayR.prepare (maxLatency (sr));

        for (int r = 0; r < 2; ++r)
        {
            auto& R = res[static_cast<size_t> (r)];
            R.n = r == 0 ? longSizeFor (sr) : shortSizeFor (sr);
            R.stft.prepare (R.n);
            R.stft.configure (R.n, r == 0 ? 1 : 0);   // long: Blackman-Harris (clean sub), short: Hann
            R.kLo = std::max (1, static_cast<int> (std::ceil (20.0 * R.n / sr)));
            const size_t B = static_cast<size_t> (R.n / 2 + 1);
            R.binFreq.assign (B, 0.0f); R.share.assign (B, 0.0f);
            R.kHi = 0;
            for (size_t k = 0; k < B; ++k)
            {
                R.binFreq[k] = static_cast<float> (k * sr / R.n);
                const float lo = lowResShare (std::max (R.binFreq[k], 1.0f), kResSplitHz);
                R.share[k] = r == 0 ? lo : 1.0f - lo;
                if (R.share[k] > 0.0f) R.kHi = static_cast<int> (k);
            }
            for (auto& m : R.mask) m.assign (B, 0.0f);
            for (int c = 0; c < 2; ++c) { R.level[c].assign (B, 0.0f); R.g1[c].assign (B, 0.0f); R.g2[c].assign (B, 0.0f); R.gOut[c].assign (B, 0.0f); }
            R.prefix.assign (B + 1, 0.0);
            R.halfW.assign (B, 0);
            // The gain may change by at most 12 dB per 100 Hz across bins: a notch of any depth
            // then has an impulse response of a few ms, well inside the frame.
            R.slopeDbPerBin = 12.0f * static_cast<float> (sr / R.n) / 100.0f;
        }
        shortIn[0].prepare (res[0].n); shortIn[1].prepare (res[0].n);
        shortSc[0].prepare (res[0].n); shortSc[1].prepare (res[0].n);

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
        for (auto& ch : det) for (auto& d : ch) d.reset();
        for (auto& ch : look) for (auto& d : ch) d.reset();
        for (auto& ch : lookSmooth) for (auto& m : ch) m.reset();
        for (int c = 0; c < 2; ++c) { gDb[c].fill (0.0f); gDb2[c].fill (0.0f); }
        offDelayL.reset(); offDelayR.reset();
        for (auto& c : clipper) c.reset();
        for (auto& R : res)
        {
            R.stft.reset();
            for (int c = 0; c < 2; ++c) { std::fill (R.g1[c].begin(), R.g1[c].end(), 0.0f); std::fill (R.g2[c].begin(), R.g2[c].end(), 0.0f); std::fill (R.gOut[c].begin(), R.gOut[c].end(), 0.0f); }
        }
        for (auto& d : shortIn) d.reset();
        for (auto& d : shortSc) d.reset();
    }

    void OttEngine::setSettings (const OttSettings& s)
    {
        if (! prepared) { st = s; return; }
        if (s == st) return;
        const bool engineChanged = s.engine != st.engine;
        const bool bandsChanged = s.x1 != st.x1 || s.x2 != st.x2;
        const bool resChanged = s.resolution != st.resolution || bandsChanged;
        const bool detChanged = bandsChanged || s.rms != st.rms || s.style != st.style;
        st = s;

        for (auto& sp : split)   sp.set (sr, st.x1, st.x2, kDynBands);
        for (auto& sp : scSplit) sp.set (sr, st.x1, st.x2, kDynBands);
        lookSamples = std::clamp (static_cast<int> (std::lround (st.lookaheadMs * 0.001 * sr)), 0, static_cast<int> (0.01 * sr));

        if (detChanged)
        {
            // Detector time per band, from the band's lowest content: the low band always holds
            // the sub (25 ms: a 40 Hz tone's ripple is under 0.05 dB), the others follow their
            // crossover. Smooth doubles it.
            const float styleMul = st.style == 1 ? 2.0f : 1.0f;
            const float tau[kDynBands] = { 0.025f, std::clamp (0.7f / std::max (st.x1, 20.0f), 0.003f, 0.015f), std::clamp (0.7f / std::max (st.x2, 300.0f), 0.0015f, 0.005f) };
            for (int c = 0; c < 2; ++c)
                for (int b = 0; b < kDynBands; ++b)
                    det[static_cast<size_t> (c)][static_cast<size_t> (b)].set (sr, tau[b] * styleMul, ! st.rms);
        }

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

        for (auto& R : res)
        {
            if (bandsChanged)
                for (int b = 0; b < kDynBands; ++b)
                {
                    double sum = 0.0;
                    for (size_t k = 0; k < R.binFreq.size(); ++k)
                    {
                        R.mask[static_cast<size_t> (b)][k] = bandMask (kDynBands, st.x1, st.x2, std::max (R.binFreq[k], 1.0f), b);
                        sum += R.mask[static_cast<size_t> (b)][k];
                    }
                    R.maskBins[static_cast<size_t> (b)] = static_cast<float> (std::max (sum, 1.0));
                }
            if (resChanged)
            {
                const float wOct = std::max ((1.0f - std::clamp (st.resolution, 0.0f, 1.0f)) * 1.0f, 0.05f);
                const float c = std::pow (2.0f, 0.5f * wOct) - 1.0f;
                for (size_t k = 0; k < R.halfW.size(); ++k) R.halfW[k] = static_cast<int> (std::floor (static_cast<float> (k) * c));
            }
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
        if (st.engine == 0) processMultiband (l, r, scl, scr);
        else
        {
            // Long resolution on the signal as is; the short one on it delayed by the difference,
            // so both outputs line up. Each applies its share of the spectrum; the sum is whole.
            const int d = res[0].n - res[1].n;
            float sl = shortIn[0].process (l, d), sr2 = shortIn[1].process (r, d);
            const float ssl = shortSc[0].process (scl, d), ssr = shortSc[1].process (scr, d);
            res[0].stft.process (l, r, scl, scr, st.sidechain, [this] (const StereoStft& s, std::array<std::vector<float>, 2>& g) { spectralFrame (res[0], s, g); });
            res[1].stft.process (sl, sr2, ssl, ssr, st.sidechain, [this] (const StereoStft& s, std::array<std::vector<float>, 2>& g) { spectralFrame (res[1], s, g); if (++vizCount >= 4) { vizCount = 0; spectralViz(); } });
            l += sl; r += sr2;
        }
        if (st.clip)
        {
            const float c = dbToLin (st.clipDb);
            l = clipper[0].process (l, c);
            r = clipper[1].process (r, c);
        }
    }

    void OttEngine::processMultiband (float& l, float& r, float scl, float scr)
    {
        const float gin = dbToLin (st.inDb), gout = dbToLin (st.outDb);
        float bands[2][kDynBands], detIn[2][kDynBands];
        const float x[2] = { l * gin, r * gin };
        for (int c = 0; c < 2; ++c) split[static_cast<size_t> (c)].process (x[c], bands[c]);
        if (st.sidechain)
        {
            const float s[2] = { scl * gin, scr * gin };
            for (int c = 0; c < 2; ++c)
            {
                if (st.scWide) for (int b = 0; b < kDynBands; ++b) detIn[c][b] = s[c];
                else scSplit[static_cast<size_t> (c)].process (s[c], detIn[c]);
            }
        }
        else
            for (int c = 0; c < 2; ++c) for (int b = 0; b < kDynBands; ++b) detIn[c][b] = bands[c][b];

        bool anySolo = false;
        for (const auto& b : st.band) anySolo = anySolo || b.solo;
        float out[2] = { 0.0f, 0.0f };

        for (int b = 0; b < kDynBands; ++b)
        {
            const auto& bp = st.band[static_cast<size_t> (b)];
            const float amt = st.amount * bp.amount;
            const float bin = dbToLin (bp.inDb * amt);
            float lv[2];
            for (int c = 0; c < 2; ++c)
                lv[c] = det[static_cast<size_t> (c)][static_cast<size_t> (b)].processDb (detIn[c][b] * bin);
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
                // Lookahead: the audio waits, and the gain is averaged over the same span, so a
                // change ramps in centred on the event instead of starting at it.
                if (lookSamples > 0) ge = lookSmooth[static_cast<size_t> (c)][static_cast<size_t> (b)].process (ge, lookSamples);
                const float y = look[static_cast<size_t> (c)][static_cast<size_t> (b)].process (bands[c][b], lookSamples);
                const float v = bp.bypass ? y : y * dbToLin (ge + (bp.inDb + bp.outDb) * amt + tone);
                if (! anySolo || bp.solo) out[c] += v;
                if (c == 0) { levelViz[static_cast<size_t> (b)].store (ll[0], std::memory_order_relaxed); gainViz[static_cast<size_t> (b)].store (bp.bypass ? 0.0f : ge, std::memory_order_relaxed); }
            }
        }
        l = out[0] * gout;
        r = out[1] * gout;
        updateViz();
    }

    void OttEngine::spectralFrame (SpecRes& R, const StereoStft& s, std::array<std::vector<float>, 2>& gain)
    {
        const int H = s.bins() - 1;
        const float hopSec = static_cast<float> (s.hopSize() / sr);
        const float resAmt = std::clamp (st.resolution, 0.0f, 1.0f);
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
        const double rmsFac = s.rmsFactor();   // sum of a sine's bin powers -> its mean square (window-dependent)
        for (int c = 0; c < 2; ++c)
        {
            const std::vector<float>& P = st.sidechain ? s.scPow[static_cast<size_t> (c)] : s.pow[static_cast<size_t> (c)];
            // Band levels, RMS-equivalent.
            double wide = 0.0;
            for (int b = 0; b < kDynBands; ++b)
            {
                double sum = 0.0;
                for (int k = 1; k <= H; ++k) sum += static_cast<double> (R.mask[static_cast<size_t> (b)][static_cast<size_t> (k)]) * P[static_cast<size_t> (k)];
                wide += sum * inLin2[static_cast<size_t> (b)];
                bandDb[static_cast<size_t> (c)][static_cast<size_t> (b)] = static_cast<float> (10.0 * std::log10 (sum * inLin2[static_cast<size_t> (b)] * rmsFac + 1e-20));
            }
            if (st.sidechain && st.scWide)
                for (auto& v : bandDb[static_cast<size_t> (c)]) v = static_cast<float> (10.0 * std::log10 (wide * rmsFac + 1e-20));

            // Each bin's own neighbourhood, scaled to what its band would read if the whole band
            // looked like it. Only where this resolution applies gain (the long one stops ~500 Hz).
            float* L = R.level[c].data();
            if (resAmt > 0.0f && ! (st.sidechain && st.scWide))
            {
                const int kPre = std::min (H, R.kHi + R.kHi / 2 + 2);   // the neighbourhoods reach past kHi
                R.prefix[0] = 0.0;
                for (int k = 0; k <= kPre; ++k)
                {
                    float inl = 0.0f;
                    for (int b = 0; b < kDynBands; ++b) inl += R.mask[static_cast<size_t> (b)][static_cast<size_t> (k)] * inLin2[static_cast<size_t> (b)];
                    R.prefix[static_cast<size_t> (k + 1)] = R.prefix[static_cast<size_t> (k)] + P[static_cast<size_t> (k)] * inl;
                }
                for (int k = 0; k <= R.kHi; ++k)
                {
                    const int h = std::max (R.halfW[static_cast<size_t> (k)], 1);
                    const int lo = std::max (1, k - h), hi = std::min (kPre, k + h);
                    const double dens = (hi >= lo) ? (R.prefix[static_cast<size_t> (hi + 1)] - R.prefix[static_cast<size_t> (lo)]) / (hi - lo + 1) : 0.0;
                    float nb = 0.0f, bd = 0.0f;
                    for (int b = 0; b < kDynBands; ++b)
                    {
                        const float w = R.mask[static_cast<size_t> (b)][static_cast<size_t> (k)];
                        nb += w * R.maskBins[static_cast<size_t> (b)];
                        bd += w * bandDb[static_cast<size_t> (c)][static_cast<size_t> (b)];
                    }
                    const float local = powerToDb (static_cast<float> (dens * nb * rmsFac));
                    L[k] = bd + (local - bd) * resAmt;
                }
            }
            else
                for (int k = 0; k <= R.kHi; ++k)
                {
                    float bd = 0.0f;
                    for (int b = 0; b < kDynBands; ++b) bd += R.mask[static_cast<size_t> (b)][static_cast<size_t> (k)] * bandDb[static_cast<size_t> (c)][static_cast<size_t> (b)];
                    L[k] = bd;
                }
        }

        // Dynamic gain per bin: the band curves blended by the masks, then attack / release.
        const int KH = R.kHi;
        for (int k = 0; k <= KH; ++k)
        {
            const float hi = std::max (R.level[0][static_cast<size_t> (k)], R.level[1][static_cast<size_t> (k)]);
            float lk[2];
            for (int c = 0; c < 2; ++c)
            {
                const float own = R.level[c][static_cast<size_t> (k)], other = R.level[1 - c][static_cast<size_t> (k)];
                lk[c] = st.link <= 1.0f ? own + (hi - own) * st.link : hi + (other - hi) * (st.link - 1.0f);
            }
            for (int c = 0; c < 2; ++c)
            {
                float t = 0.0f, a = 0.0f, rr = 0.0f;
                for (int b = 0; b < kDynBands; ++b)
                {
                    const float w = R.mask[static_cast<size_t> (b)][static_cast<size_t> (k)];
                    if (w <= 0.0f) continue;
                    t += w * computeGain (st.band[static_cast<size_t> (b)], lk[c], amt[static_cast<size_t> (b)]);
                    a += w * fa[static_cast<size_t> (b)]; rr += w * fr[static_cast<size_t> (b)];
                }
                float& g1 = R.g1[c][static_cast<size_t> (k)];
                g1 += ((t < g1) ? a : rr) * (t - g1);
                float g = g1;
                if (st.style == 1)   // Smooth: a second stage
                {
                    float& g2 = R.g2[c][static_cast<size_t> (k)];
                    g2 += ((g1 < g2) ? a : rr) * (g1 - g2);
                    g = g2;
                }
                R.gOut[c][static_cast<size_t> (k)] = g;
            }
        }

        // Keep the gain curve's slope across bins within bounds: the lower envelope with V-shaped
        // sides (a cut spreads to its neighbours, a lift never pulls a cut up), then take the edge
        // off. The equivalent filter stays a few ms long, inside the frame, so nothing wraps.
        // Bins under 20 Hz follow the first one above (DC leakage flickers; it must not decide).
        const float sl = R.slopeDbPerBin;
        for (int c = 0; c < 2; ++c)
        {
            float* g = R.gOut[c].data();
            for (int k = 0; k < R.kLo && k < KH; ++k) g[k] = g[R.kLo];
            for (int k = 1; k <= KH; ++k) g[k] = std::min (g[k], g[k - 1] + sl);
            for (int k = KH - 1; k >= 0; --k) g[k] = std::min (g[k], g[k + 1] + sl);
            float prev = g[0];
            for (int k = 1; k < KH; ++k) { const float cur = g[k]; g[k] = 0.25f * prev + 0.5f * cur + 0.25f * g[k + 1]; prev = cur; }
        }

        // Static gains (in / out / tone / band in-out), solo and bypass, this resolution's share.
        std::array<float, kDynBands> staticLin {};
        for (int b = 0; b < kDynBands; ++b)
        {
            const auto& bp = st.band[static_cast<size_t> (b)];
            const float tone = (b == 0 ? st.lowTone : (b == kDynBands - 1 ? st.highTone : 0.0f));
            staticLin[static_cast<size_t> (b)] = (anySolo && ! bp.solo) ? 0.0f : dbToLin (st.inDb + st.outDb + (bp.inDb + bp.outDb) * amt[static_cast<size_t> (b)] + tone);
        }
        for (int c = 0; c < 2; ++c)
        {
            float* out = gain[static_cast<size_t> (c)].data();
            for (int k = 0; k <= KH; ++k)
            {
                const float gl = dbToLin (R.gOut[c][static_cast<size_t> (k)]);
                float lin = 0.0f;
                for (int b = 0; b < kDynBands; ++b)
                {
                    const float w = R.mask[static_cast<size_t> (b)][static_cast<size_t> (k)];
                    if (w <= 0.0f) continue;
                    const auto& bp = st.band[static_cast<size_t> (b)];
                    lin += w * (bp.bypass ? ((anySolo && ! bp.solo) ? 0.0f : 1.0f) : gl * staticLin[static_cast<size_t> (b)]);
                }
                out[k] = lin * R.share[static_cast<size_t> (k)];
            }
            for (int k = KH + 1; k <= H; ++k) out[k] = 0.0f;
        }

        if (&R == &res[1])
            for (int b = 0; b < kDynBands; ++b)
                levelViz[static_cast<size_t> (b)].store (bandDb[0][static_cast<size_t> (b)], std::memory_order_relaxed);
    }

    void OttEngine::spectralViz()
    {
        // Both resolutions blended by their shares: per display point, and per band (mean).
        const auto& Lr = res[0];
        const auto& Sr = res[1];
        const int HL = Lr.n / 2, HS = Sr.n / 2;
        for (int p = 0; p < kCurvePoints; ++p)
        {
            const float f = curvePointFreq (p);
            const int kl = std::clamp (static_cast<int> (std::lround (f * Lr.n / sr)), 0, HL);
            const int ks = std::clamp (static_cast<int> (std::lround (f * Sr.n / sr)), 0, HS);
            const float lo = lowResShare (f, kResSplitHz);
            viz[static_cast<size_t> (p)].store (lo * Lr.gOut[0][static_cast<size_t> (kl)] + (1.0f - lo) * Sr.gOut[0][static_cast<size_t> (ks)], std::memory_order_relaxed);
        }
        for (int b = 0; b < kDynBands; ++b)
        {
            double s2 = 0.0, w2 = 0.0;
            for (int r = 0; r < 2; ++r)
            {
                const auto& R = res[static_cast<size_t> (r)];
                const int H = R.n / 2;
                for (int k = 0; k <= H; ++k)
                {
                    const double w = static_cast<double> (R.mask[static_cast<size_t> (b)][static_cast<size_t> (k)]) * R.share[static_cast<size_t> (k)];
                    s2 += w * R.gOut[0][static_cast<size_t> (k)]; w2 += w;
                }
            }
            gainViz[static_cast<size_t> (b)].store (static_cast<float> (s2 / std::max (w2, 1e-9)), std::memory_order_relaxed);
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
