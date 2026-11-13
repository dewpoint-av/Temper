#include "SpectralDynamics.h"
#include "FastMath.h"

#include <algorithm>
#include <cmath>

namespace temper
{
    namespace
    {
        constexpr float kLn10Over20 = 0.11512925465f;   // ln(10) / 20
        constexpr float kKneeDb     = 6.0f;
        constexpr double kPi        = 3.14159265358979323846;

        int srScale (double sampleRate)
        {
            int scale = 1;
            while (sampleRate / (48000.0 * scale) > 1.5) scale *= 2;   // 88.2 / 96 k -> 2, 176.4 / 192 k -> 4
            return scale;
        }
        // 0 below -knee/2, quadratic through the knee, the excess itself above it.
        float softOver (float over, float knee)
        {
            if (over <= -0.5f * knee) return 0.0f;
            if (over < 0.5f * knee) { const float t = over + 0.5f * knee; return t * t / (2.0f * knee); }
            return over;
        }
    }

    // =====================================================================================
    // DynCore
    // =====================================================================================

    void DynCore::prepare (int maxSize, double sampleRate)
    {
        sr = sampleRate;
        maxN = maxSize;
        const size_t N = static_cast<size_t> (maxN), B = N / 2 + 1;
        for (int c = 0; c < 2; ++c)
        {
            inBuf[c].assign (N, 0.0f); scBuf[c].assign (N, 0.0f); outAcc[c].assign (N, 0.0f);
            xr[c].assign (B, 0.0f); xi[c].assign (B, 0.0f); det[c].assign (B, 0.0f);
            gr[c].assign (B, 0.0f); grS[c].assign (B, 0.0f); gainRe[c].assign (B, 1.0f); gainIm[c].assign (B, 0.0f);
            detPow[c].assign (B, 0.0f);
        }
        re.assign (N, 0.0f); im.assign (N, 0.0f); sre.assign (N, 0.0f); sim.assign (N, 0.0f);
        window.assign (N, 0.0f);
        binFreq.assign (B, 0.0f); log2f.assign (B, 0.0f); share.assign (B, 0.0f);
        tmpA.assign (B, 0.0f); tmpB.assign (B, 0.0f); ltAvg.assign (B, 0.0f); adaptCurve.assign (B, 0.0f);
        preW.assign (B, 0.0f); customBin.assign (B, 0.0f); ltPow.assign (B, 0.0f);
        spreadHalf.assign (B, 0); dqBuf.assign (B, 0); prefix.assign (B + 1, 0.0);
        learnSum.assign (B, 0.0);
        n = 0;
    }

    void DynCore::configure (int size, int windowType, bool lowPart, float splitHz)
    {
        n = std::min (size, maxN);
        hop = n / 8;
        half = n / 2;
        fft.init (n);
        double sumW = 0.0;
        for (int i = 0; i < n; ++i)
        {
            const double a = 2.0 * kPi * i / n;
            const double w = windowType == 1 ? 0.35875 - 0.48829 * std::cos (a) + 0.14128 * std::cos (2.0 * a) - 0.01168 * std::cos (3.0 * a)
                                             : 0.5 - 0.5 * std::cos (a);
            window[static_cast<size_t> (i)] = static_cast<float> (w);
            sumW += w;
        }
        // Overlap-add of window^2 at this hop is constant (both windows at 8x): its value at any sample.
        double s = 0.0;
        for (int m = 0; m < n / hop; ++m) { const double w = window[static_cast<size_t> ((hop / 2 + m * hop) % n)]; s += w * w; }
        olaScale = static_cast<float> (1.0 / (n * s));
        powNorm = static_cast<float> ((2.0 / sumW) * (2.0 / sumW));   // a full-scale sine reads 1 in its bin

        kShareLo = half + 1; kShareHi = -1;
        for (int k = 0; k <= half; ++k)
        {
            const float f = static_cast<float> (k * sr / n);
            binFreq[static_cast<size_t> (k)] = f;
            log2f[static_cast<size_t> (k)] = std::log2 (std::max (f, 10.0f) / 1000.0f);
            const float lo = lowResShare (std::max (f, 1.0f), splitHz);
            const float sh = lowPart ? lo : 1.0f - lo;
            share[static_cast<size_t> (k)] = sh;
            if (sh > 0.0f) { kShareLo = std::min (kShareLo, k); kShareHi = std::max (kShareHi, k); }
        }
        kLo = std::max (1, static_cast<int> (std::ceil (10.0 * n / sr)));
        kHi = std::min (half, static_cast<int> (std::floor (std::min (22000.0, 0.5 * sr) * n / sr)));
        // Display points: each covers the bins between its neighbours' geometric midpoints.
        for (int p = 0; p <= kCurvePoints; ++p)
        {
            const float edge = 20.0f * std::pow (1000.0f, (static_cast<float> (p) - 0.5f) / static_cast<float> (kCurvePoints - 1));
            vizBin[static_cast<size_t> (p)] = std::clamp (static_cast<int> (std::lround (edge * n / sr)), 1, half);
        }
        reset();
        for (int c = 0; c < 2; ++c) { std::fill (gr[c].begin(), gr[c].end(), 0.0f); std::fill (gainRe[c].begin(), gainRe[c].end(), 1.0f); std::fill (gainIm[c].begin(), gainIm[c].end(), 0.0f); }
        rebuildPreWeights();
        rebuildCustomBins();
        rebuildSpread();
    }

    void DynCore::reset()
    {
        for (int c = 0; c < 2; ++c)
        {
            std::fill (inBuf[c].begin(), inBuf[c].end(), 0.0f);
            std::fill (scBuf[c].begin(), scBuf[c].end(), 0.0f);
            std::fill (outAcc[c].begin(), outAcc[c].end(), 0.0f);
            std::fill (grS[c].begin(), grS[c].end(), 0.0f);
        }
        wpos = rpos = hopCount = 0;
        ltInit = false;
        detInit = false;
        tilt = 0.0f;
    }

    void DynCore::setSettings (const DynSettings& s)
    {
        if (n == 0) { st = s; return; }
        if (s == st) return;
        const bool pre = s.pre != st.pre;
        const bool sel = s.selectivity != st.selectivity;
        const bool ms  = s.midSide != st.midSide;
        st = s;
        if (pre) rebuildPreWeights();
        if (sel) rebuildSpread();
        if (ms) { for (int c = 0; c < 2; ++c) std::fill (grS[c].begin(), grS[c].end(), 0.0f); }
    }

    void DynCore::rebuildPreWeights()
    {
        for (int k = 0; k <= half; ++k)
        {
            float w = 0.0f;
            for (const auto& nd : st.pre)
                if (nd.on) w += nodeResponseDb (nd, sr, std::max (binFreq[static_cast<size_t> (k)], 1.0f));
            preW[static_cast<size_t> (k)] = std::clamp (w, -60.0f, 40.0f);
        }
    }

    void DynCore::rebuildSpread()
    {
        // Selectivity 0: a cut spreads over an octave; 0.5: a quarter; 1: the bin alone.
        const float s = 1.0f - std::clamp (st.selectivity, 0.0f, 1.0f);
        const float widthOct = s * s;
        const float c = std::pow (2.0f, 0.5f * widthOct) - 1.0f;
        for (int k = 0; k <= half; ++k)
            spreadHalf[static_cast<size_t> (k)] = static_cast<int> (std::floor (static_cast<float> (k) * c));
    }

    void DynCore::rebuildCustomBins()
    {
        for (int k = 0; k <= half; ++k)
        {
            const float f = std::max (binFreq[static_cast<size_t> (k)], 20.0f);
            const float x = std::log (f / 20.0f) / std::log (1000.0f) * static_cast<float> (kCurvePoints - 1);
            const int i0 = std::clamp (static_cast<int> (std::floor (x)), 0, kCurvePoints - 1);
            const int i1 = std::min (i0 + 1, kCurvePoints - 1);
            const float t = std::clamp (x - static_cast<float> (i0), 0.0f, 1.0f);
            customBin[static_cast<size_t> (k)] = curve[static_cast<size_t> (i0)] * (1.0f - t) + curve[static_cast<size_t> (i1)] * t;
        }
    }

    void DynCore::getCustomCurve (float* dB) const
    {
        for (int i = 0; i < kCurvePoints; ++i) dB[i] = curve[static_cast<size_t> (i)];
    }

    void DynCore::setCustomCurve (const float* dB)
    {
        for (int i = 0; i < kCurvePoints; ++i) pendingCurve[static_cast<size_t> (i)] = dB[i];
        pendingSet.store (true);
    }

    // Min over each bin's window (a monotonic deque: both window ends only move up), then the
    // average of that over the same window: a cut keeps its depth and gets soft shoulders.
    void DynCore::spread (float* g, float* tmp) const
    {
        int head = 0, tail = 0, next = 0;
        int* dq = const_cast<int*> (dqBuf.data());
        double* P = const_cast<double*> (prefix.data());
        for (int k = 0; k <= half; ++k)
        {
            const int h = spreadHalf[static_cast<size_t> (k)];
            const int lo = std::max (0, k - h), hi = std::min (half, k + h);
            while (next <= hi)
            {
                while (tail > head && g[dq[tail - 1]] >= g[next]) --tail;
                dq[tail++] = next++;
            }
            while (dq[head] < lo) ++head;
            tmp[k] = g[dq[head]];
        }
        P[0] = 0.0;
        for (int k = 0; k <= half; ++k) P[k + 1] = P[k] + tmp[k];
        for (int k = 0; k <= half; ++k)
        {
            const int h = spreadHalf[static_cast<size_t> (k)];
            const int lo = std::max (0, k - h), hi = std::min (half, k + h);
            g[k] = static_cast<float> ((P[hi + 1] - P[lo]) / (hi - lo + 1));
        }
    }

    void DynCore::process (float& l, float& r, float scl, float scr)
    {
        inBuf[0][static_cast<size_t> (wpos)] = l;
        inBuf[1][static_cast<size_t> (wpos)] = r;
        scBuf[0][static_cast<size_t> (wpos)] = scl;
        scBuf[1][static_cast<size_t> (wpos)] = scr;
        wpos = (wpos + 1) & (n - 1);

        l = outAcc[0][static_cast<size_t> (rpos)]; r = outAcc[1][static_cast<size_t> (rpos)];
        outAcc[0][static_cast<size_t> (rpos)] = 0.0f;
        outAcc[1][static_cast<size_t> (rpos)] = 0.0f;
        rpos = (rpos + 1) & (n - 1);

        if (++hopCount >= hop) { hopCount = 0; frame(); }
    }

    void DynCore::frame()
    {
        const int N = n, H = half;
        const size_t mask = static_cast<size_t> (N - 1);

        // ---- Learn / custom curve requests --------------------------------------------
        if (const int req = learnRequest.exchange (0))
        {
            if (req == 1) { std::fill (learnSum.begin(), learnSum.end(), 0.0); learnFrames = 0; learning.store (true); }
            else if (req == 2 && learning.load())
            {
                learning.store (false);
                if (learnFrames > 0)
                {
                    std::array<float, kCurvePoints> raw {};
                    for (int p = 0; p < kCurvePoints; ++p)
                    {
                        const int lo = vizBin[static_cast<size_t> (p)], hi = std::max (lo, vizBin[static_cast<size_t> (p + 1)] - 1);
                        double s = 0.0;
                        for (int k = lo; k <= hi; ++k) s += learnSum[static_cast<size_t> (k)];
                        raw[static_cast<size_t> (p)] = static_cast<float> (10.0 * std::log10 (s / ((hi - lo + 1) * static_cast<double> (learnFrames)) + 1e-24));
                    }
                    // Smooth over a third of an octave, then normalise its 100 Hz - 10 kHz mean to 0 dB.
                    const int w = 4;
                    double mean = 0.0; int cnt = 0;
                    for (int p = 0; p < kCurvePoints; ++p)
                    {
                        double s = 0.0; int c = 0;
                        for (int j = std::max (0, p - w); j <= std::min (kCurvePoints - 1, p + w); ++j) { s += raw[static_cast<size_t> (j)]; ++c; }
                        curve[static_cast<size_t> (p)] = static_cast<float> (s / c);
                        const float f = curvePointFreq (p);
                        if (f >= 100.0f && f <= 10000.0f) { mean += curve[static_cast<size_t> (p)]; ++cnt; }
                    }
                    for (auto& v : curve) v -= static_cast<float> (mean / std::max (cnt, 1));
                    curveValid.store (true);
                    newCurve.store (true);
                    rebuildCustomBins();
                }
            }
        }
        if (pendingSet.exchange (false)) { curve = pendingCurve; curveValid.store (true); rebuildCustomBins(); }
        if (clearRequest.exchange (false)) curveValid.store (false);

        // ---- Gather the windowed frame (both channels in one complex FFT) --------------
        for (int i = 0; i < N; ++i)
        {
            const size_t idx = (static_cast<size_t> (wpos) + static_cast<size_t> (i)) & mask;
            const float w = window[static_cast<size_t> (i)];
            re[static_cast<size_t> (i)] = inBuf[0][idx] * w;
            im[static_cast<size_t> (i)] = inBuf[1][idx] * w;
        }
        fft.transform (re.data(), im.data(), false);
        // Unpack: L = (Z[k] + conj Z[N-k]) / 2, R = (Z[k] - conj Z[N-k]) / 2i.
        for (int k = 0; k <= H; ++k)
        {
            const size_t a = static_cast<size_t> (k), b = static_cast<size_t> ((N - k) & (N - 1));
            xr[0][a] = 0.5f * (re[a] + re[b]); xi[0][a] = 0.5f * (im[a] - im[b]);
            xr[1][a] = 0.5f * (im[a] + im[b]); xi[1][a] = -0.5f * (re[a] - re[b]);
        }

        // ---- Detector: power per bin (a full-scale sine reads 0 dB) -----------------------
        float scGain = 1.0f;
        const float* dRe[2] = { xr[0].data(), xr[1].data() };
        const float* dIm[2] = { xi[0].data(), xi[1].data() };
        if (st.sidechain)
        {
            for (int i = 0; i < N; ++i)
            {
                const size_t idx = (static_cast<size_t> (wpos) + static_cast<size_t> (i)) & mask;
                const float w = window[static_cast<size_t> (i)];
                sre[static_cast<size_t> (i)] = scBuf[0][idx] * w;
                sim[static_cast<size_t> (i)] = scBuf[1][idx] * w;
            }
            fft.transform (sre.data(), sim.data(), false);
            // Unpack into gainRe / gainIm (free until the gains are computed).
            for (int k = 0; k <= H; ++k)
            {
                const size_t a = static_cast<size_t> (k), b = static_cast<size_t> ((N - k) & (N - 1));
                gainRe[0][a] = 0.5f * (sre[a] + sre[b]); gainIm[0][a] = 0.5f * (sim[a] - sim[b]);
                gainRe[1][a] = 0.5f * (sim[a] + sim[b]); gainIm[1][a] = -0.5f * (sre[a] - sre[b]);
            }
            dRe[0] = gainRe[0].data(); dRe[1] = gainRe[1].data(); dIm[0] = gainIm[0].data(); dIm[1] = gainIm[1].data();
            scGain = std::pow (10.0f, st.scGainDb / 20.0f);
        }
        // The bins this core acts on: its share of the spectrum, within 10 Hz .. 22 kHz. dB
        // conversions (the costly part) happen only where they are read.
        const int kA = std::max (kLo, kShareLo), kB = std::min (kHi, kShareHi);
        const bool needTilt = st.adaptiveSlope && st.thrMode != 1;
        int ltA = kA, ltB = kB;                                     // where the long-term dB curve is needed
        if (st.thrMode == 1) { ltA = std::max (1, static_cast<int> (kA / 1.42f)); ltB = std::min (H, static_cast<int> (kB * 1.42f) + 1); }
        if (needTilt) { ltA = std::min (ltA, std::max (1, static_cast<int> (100.0 * N / sr))); ltB = std::max (ltB, std::min (H, static_cast<int> (10000.0 * N / sr))); }

        const float pScale = powNorm * scGain * scGain;
        double framePow = 0.0;
        for (int c = 0; c < 2; ++c)
        {
            float* p = tmpA.data();
            for (int k = 0; k <= H; ++k)
                p[k] = (dRe[c][k] * dRe[c][k] + dIm[c][k] * dIm[c][k]) * pScale;
            if (learning.load()) for (int k = 0; k <= H; ++k) learnSum[static_cast<size_t> (k)] += 0.5 * p[k];
            // Noise reads differently in every bin and frame (a single bin's power is about
            // exponentially distributed): average 5 bins and blend with the last frame, so
            // only what really stands out triggers.
            float* dp = detPow[c].data();
            for (int k = 0; k <= H; ++k)
            {
                framePow += p[k];
                const float sm = 0.1f * p[std::max (k - 2, 0)] + 0.2f * p[std::max (k - 1, 0)] + 0.4f * p[k]
                               + 0.2f * p[std::min (k + 1, H)] + 0.1f * p[std::min (k + 2, H)];
                dp[k] = detInit ? 0.5f * (dp[k] + sm) : sm;
            }
            for (int k = kA; k <= kB; ++k) det[c][static_cast<size_t> (k)] = powerToDb (dp[k]);
        }
        if (learning.load()) ++learnFrames;
        detInit = true;

        // ---- Long-term spectrum: adaptive threshold and slope ---------------------------
        const float hopSec = static_cast<float> (hop / sr);
        if (framePow > 1.0e-10)   // not silence: silence would drag the average down
        {
            const float ltCoef = 1.0f - std::exp (-hopSec / 2.0f);
            // Averaged as power (an average of dB sits about 2.5 dB under it for noise).
            for (int k = 0; k <= H; ++k)
            {
                const float m = 0.5f * (detPow[0][static_cast<size_t> (k)] + detPow[1][static_cast<size_t> (k)]);
                float& v = ltPow[static_cast<size_t> (k)];
                v = ltInit ? v + ltCoef * (m - v) : m;
            }
            for (int k = ltA; k <= ltB; ++k) ltAvg[static_cast<size_t> (k)] = powerToDb (ltPow[static_cast<size_t> (k)]);
            ltInit = true;
            if (needTilt)
            {
                // Spectral tilt (dB / octave), each octave from 100 Hz to 10 kHz weighted alike.
                double sw = 0, sx = 0, sy = 0, sxx = 0, sxy = 0;
                for (int k = 1; k <= H; ++k)
                {
                    const float f = binFreq[static_cast<size_t> (k)];
                    if (f < 100.0f || f > 10000.0f) continue;
                    const double w = 1.0 / f, x = log2f[static_cast<size_t> (k)], y = ltAvg[static_cast<size_t> (k)];
                    sw += w; sx += w * x; sy += w * y; sxx += w * x * x; sxy += w * x * y;
                }
                const double var = sxx / sw - (sx / sw) * (sx / sw);
                if (sw > 0.0 && var > 1e-9) tilt = static_cast<float> ((sxy / sw - (sx / sw) * (sy / sw)) / var);
            }
        }
        float slopeEff = st.slopeDbOct + (needTilt ? -tilt : 0.0f);
        slopeEff = std::clamp (slopeEff, -12.0f, 12.0f);
        slopeViz.store (slopeEff, std::memory_order_relaxed);
        if (st.thrMode == 1)
        {
            // The long-term spectrum averaged over an octave around each bin.
            double* P = prefix.data();
            P[ltA] = 0.0;
            for (int k = ltA; k <= ltB; ++k) P[k + 1] = P[k] + ltAvg[static_cast<size_t> (k)];
            for (int k = kA; k <= kB; ++k)
            {
                const int h = static_cast<int> (static_cast<float> (k) * 0.41421356f);
                const int lo = std::max (ltA, k - h), hi = std::min (ltB, k + h);
                adaptCurve[static_cast<size_t> (k)] = (hi >= lo) ? static_cast<float> ((P[hi + 1] - P[lo]) / (hi - lo + 1)) : ltAvg[static_cast<size_t> (k)];
            }
        }

        // ---- Gain reduction per bin -------------------------------------------------------
        const bool custom = st.thrMode == 2 && curveValid.load();
        const float capKnee = std::clamp (0.5f * st.maxGrDb, 0.1f, 3.0f);   // ease into Max GR instead of a hard stop
        for (int c = 0; c < 2; ++c)
        {
            float* g = gr[c].data();
            const float* d = det[c].data();
            std::fill (g, g + H + 1, 0.0f);
            for (int k = kA; k <= kB; ++k)
            {
                float thr = st.thresholdDb;
                if (st.thrMode == 1) thr += adaptCurve[static_cast<size_t> (k)];
                else if (custom)     thr += customBin[static_cast<size_t> (k)];
                const float over = d[k] + preW[static_cast<size_t> (k)] + slopeEff * log2f[static_cast<size_t> (k)] - thr;
                const float x = st.strength * softOver (over, kKneeDb);
                g[k] = -(x - softOver (x - st.maxGrDb, capKnee));
            }
        }
        // Link: each channel moves toward the deeper of the two.
        if (st.link > 0.0f)
            for (int k = kA; k <= kB; ++k)
            {
                const float m = std::min (gr[0][static_cast<size_t> (k)], gr[1][static_cast<size_t> (k)]);
                gr[0][static_cast<size_t> (k)] += (m - gr[0][static_cast<size_t> (k)]) * st.link;
                gr[1][static_cast<size_t> (k)] += (m - gr[1][static_cast<size_t> (k)]) * st.link;
            }
        // Selectivity, then attack / release per bin.
        const float att = 1.0f - std::exp (-hopSec / std::max (st.attackMs * 0.001f, 1.0e-4f));
        const float rel = 1.0f - std::exp (-hopSec / std::max (st.releaseMs * 0.001f, 1.0e-4f));
        float maxGr = 0.0f;
        for (int c = 0; c < 2; ++c)
        {
            spread (gr[c].data(), tmpB.data());
            float* s = grS[c].data();
            const float* g = gr[c].data();
            for (int k = 0; k <= H; ++k)
            {
                s[k] += ((g[k] < s[k]) ? att : rel) * (g[k] - s[k]);
                maxGr = std::min (maxGr, s[k]);
            }
        }
        maxGrViz.store (maxGr, std::memory_order_relaxed);

        // ---- Gains: the GR (or, monitoring, the detector's weighting) -------------------
        const bool useGr = ! monitorDet;
        for (int c = 0; c < 2; ++c)
            for (int k = 0; k <= H; ++k)
                gainRe[c][static_cast<size_t> (k)] = useGr ? grS[c][static_cast<size_t> (k)]
                                                           : std::clamp (preW[static_cast<size_t> (k)] + slopeEff * log2f[static_cast<size_t> (k)], -40.0f, 30.0f);

        if (st.naturalPhase && useGr)
        {
            // Minimum phase from the log magnitude (homomorphic method): real cepstrum, fold
            // its anticausal half onto the causal one, back to the spectrum, exponentiate.
            for (int k = 0; k <= H; ++k)
            {
                const float aL = gainRe[0][static_cast<size_t> (k)] * kLn10Over20, aR = gainRe[1][static_cast<size_t> (k)] * kLn10Over20;
                re[static_cast<size_t> (k)] = aL; im[static_cast<size_t> (k)] = aR;
                if (k > 0 && k < H) { re[static_cast<size_t> (N - k)] = aL; im[static_cast<size_t> (N - k)] = aR; }
            }
            fft.transform (re.data(), im.data(), true);
            const float invN = 1.0f / static_cast<float> (N);
            for (int t = 0; t < N; ++t)
            {
                const float f = (t == 0 || t == H) ? invN : (t < H ? 2.0f * invN : 0.0f);
                re[static_cast<size_t> (t)] *= f; im[static_cast<size_t> (t)] *= f;
            }
            fft.transform (re.data(), im.data(), false);
            for (int k = 0; k <= H; ++k)
            {
                const size_t a = static_cast<size_t> (k), b = static_cast<size_t> ((N - k) & (N - 1));
                const float Ar = 0.5f * (re[a] + re[b]), Ai = 0.5f * (im[a] - im[b]);
                const float Br = 0.5f * (im[a] + im[b]), Bi = -0.5f * (re[a] - re[b]);
                const float mL = std::exp (Ar) * share[a], mR = std::exp (Br) * share[a];
                gainRe[0][a] = mL * std::cos (Ai); gainIm[0][a] = mL * std::sin (Ai);
                gainRe[1][a] = mR * std::cos (Bi); gainIm[1][a] = mR * std::sin (Bi);
            }
        }
        else
        {
            for (int c = 0; c < 2; ++c)
                for (int k = 0; k <= H; ++k)
                {
                    const float sh = share[static_cast<size_t> (k)];
                    gainRe[c][static_cast<size_t> (k)] = sh > 0.0f ? std::exp (gainRe[c][static_cast<size_t> (k)] * kLn10Over20) * sh : 0.0f;
                    gainIm[c][static_cast<size_t> (k)] = 0.0f;
                }
        }

        // ---- Apply, repack both channels, back to time, overlap-add ----------------------
        for (int k = 0; k <= H; ++k)
        {
            const size_t a = static_cast<size_t> (k);
            const float lr = xr[0][a] * gainRe[0][a] - xi[0][a] * gainIm[0][a];
            const float li = xr[0][a] * gainIm[0][a] + xi[0][a] * gainRe[0][a];
            const float rr = xr[1][a] * gainRe[1][a] - xi[1][a] * gainIm[1][a];
            const float ri = xr[1][a] * gainIm[1][a] + xi[1][a] * gainRe[1][a];
            // Z[k] = L + iR; Z[N-k] = conj L + i conj R.
            re[a] = lr - ri; im[a] = li + rr;
            if (k > 0 && k < H)
            {
                const size_t b = static_cast<size_t> (N - k);
                re[b] = lr + ri; im[b] = rr - li;
            }
        }
        fft.transform (re.data(), im.data(), true);
        for (int i = 0; i < N; ++i)
        {
            const size_t o = (static_cast<size_t> (rpos) + static_cast<size_t> (i)) & mask;
            const float w = window[static_cast<size_t> (i)] * olaScale;
            outAcc[0][o] += re[static_cast<size_t> (i)] * w;
            outAcc[1][o] += im[static_cast<size_t> (i)] * w;
        }

        // ---- Display: the deepest GR within each display point ---------------------------
        for (int p = 0; p < kCurvePoints; ++p)
        {
            const int lo = vizBin[static_cast<size_t> (p)], hi = std::max (lo, vizBin[static_cast<size_t> (p + 1)] - 1);
            float v = 0.0f;
            for (int k = lo; k <= hi; ++k) v = std::min (v, 0.5f * (grS[0][static_cast<size_t> (k)] + grS[1][static_cast<size_t> (k)]));
            grViz[static_cast<size_t> (p)].store (useGr ? v : 0.0f, std::memory_order_relaxed);
        }
    }

    // =====================================================================================
    // SpectralDynamics: two cores, the dry path, the fade
    // =====================================================================================

    int SpectralDynamics::fftSizeFor (int quality, double sampleRate)
    {
        static const int base[3] = { 2048, 4096, 8192 };
        return base[std::clamp (quality, 0, 2)] * srScale (sampleRate);
    }

    int SpectralDynamics::shortSizeFor (int longSize, double sampleRate)
    {
        return std::max (longSize / 4, 1024 * srScale (sampleRate));
    }

    void SpectralDynamics::prepare (double sampleRate)
    {
        sr = sampleRate;
        maxN = fftSizeFor (2, sr);
        core[0].prepare (maxN, sr);
        core[1].prepare (shortSizeFor (maxN, sr), sr);
        for (auto& d : dry) d.prepare (maxN);
        for (auto& d : shortIn) d.prepare (maxN);
        for (auto& d : shortSc) d.prepare (maxN);
        for (int p = 0; p < kCurvePoints; ++p) vizLowShare[static_cast<size_t> (p)] = lowResShare (curvePointFreq (p), kSplitHz);
        configure();
    }

    void SpectralDynamics::configure()
    {
        n = fftSizeFor (st.quality, sr);
        nShort = shortSizeFor (n, sr);
        core[0].configure (n, 1, true, kSplitHz);        // Blackman-Harris: the lows, leakage-free
        core[1].configure (nShort, 0, false, kSplitHz);  // Hann: the highs, quick
        for (auto& c : core) { c.setSettings (st); c.setMonitorDetector (monitorDet); }
        reset();
    }

    void SpectralDynamics::reset()
    {
        for (auto& c : core) c.reset();
        for (auto& d : dry) d.reset();
        for (auto& d : shortIn) d.reset();
        for (auto& d : shortSc) d.reset();
        active = false; warm = 0; xf = 0.0f;
    }

    void SpectralDynamics::setSettings (const DynSettings& s)
    {
        if (maxN == 0) { st = s; return; }     // not prepared yet: prepare() configures from st
        if (s == st && n > 0) return;
        const bool reconf = (s.quality != st.quality) || n == 0;
        st = s;
        if (reconf) { configure(); return; }
        for (auto& c : core) c.setSettings (st);
    }

    void SpectralDynamics::setMonitorDetector (bool on)
    {
        monitorDet = on;
        for (auto& c : core) c.setMonitorDetector (on);
    }

    float SpectralDynamics::grAt (int p) const
    {
        const float lo = vizLowShare[static_cast<size_t> (p)];
        return lo * core[0].grAt (p) + (1.0f - lo) * core[1].grAt (p);
    }

    void SpectralDynamics::process (float& l, float& r, float scl, float scr)
    {
        if (st.midSide)
        {
            const float m = 0.5f * (l + r), s = 0.5f * (l - r);
            l = m; r = s;
            const float sm = 0.5f * (scl + scr), ss = 0.5f * (scl - scr);
            scl = sm; scr = ss;
        }
        const float dl = dry[0].process (l, n), dr = dry[1].process (r, n);

        const bool want = st.enabled || monitorDet;
        if (want && ! active)
        {
            for (auto& c : core) c.reset();
            for (auto& d : shortIn) d.reset();
            for (auto& d : shortSc) d.reset();
            active = true; warm = n; xf = 0.0f;
        }
        float ol = dl, orr = dr;
        if (active)
        {
            // The long core on the signal as is, the short one on it delayed by the difference.
            const int d = n - nShort;
            float pl = l, pr = r;
            float sl = shortIn[0].process (l, d), sr2 = shortIn[1].process (r, d);
            const float ssl = shortSc[0].process (scl, d), ssr = shortSc[1].process (scr, d);
            core[0].process (pl, pr, scl, scr);
            core[1].process (sl, sr2, ssl, ssr);
            pl += sl; pr += sr2;
            if (core[0].takeNewCurve()) { float cv[kCurvePoints]; core[0].getCustomCurve (cv); core[1].setCustomCurve (cv); }

            // Fade in once the transforms are warm (their first frames are partial); fade out
            // before stopping. Both paths carry the same delay.
            const float step = 2.0f / static_cast<float> (n);
            if (want) { if (warm > 0) --warm; else xf = std::min (1.0f, xf + step); }
            else      { xf -= step; if (xf <= 0.0f) { xf = 0.0f; active = false; } }
            ol = dl + (pl - dl) * xf;
            orr = dr + (pr - dr) * xf;
        }

        if (st.midSide) { l = ol + orr; r = ol - orr; }
        else            { l = ol; r = orr; }
    }
} // namespace temper
