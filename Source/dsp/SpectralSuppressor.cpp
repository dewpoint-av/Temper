#include "SpectralSuppressor.h"

namespace temper
{
    namespace
    {
        void orderFromIndex (int idx, int out[3])
        {
            static const int table[6][3] = { { 0, 1, 2 }, { 0, 2, 1 }, { 1, 0, 2 }, { 1, 2, 0 }, { 2, 0, 1 }, { 2, 1, 0 } };
            idx = std::clamp (idx, 0, 5);
            out[0] = table[idx][0]; out[1] = table[idx][1]; out[2] = table[idx][2];
        }
        int nextPow2 (int v) { int p = 1; while (p < v) p <<= 1; return p; }
    }

    void SpectralSuppressor::prepare (double sr, int /*maxBlockSize*/)
    {
        sampleRate = sr;
        vizInputBuf.assign (kVizSize, 0.0f);
        vizWrite.store (0);
        dyn.prepare (sr);
        ott.prepare (sr);
        gate.prepare (sr);
        const size_t ring = static_cast<size_t> (nextPow2 (SpectralDynamics::fftSizeFor (2, sr) + OttEngine::maxLatency (sr) + GateEngine::maxLatency (sr) + 1));
        for (int c = 0; c < 2; ++c) { dryDelay[c].assign (ring, 0.0f); scDelay[c].assign (ring, 0.0f); }
        agc.prepare (sr);
        update (sr);
        reset();
    }

    void SpectralSuppressor::reset()
    {
        dyn.reset();
        ott.reset();
        gate.reset();
        for (auto& b : eqBands) for (auto& ch : b.f) for (auto& f : ch) f.reset();
        for (auto& f : listenFilter) f.reset();
        for (int c = 0; c < 2; ++c) { std::fill (dryDelay[c].begin(), dryDelay[c].end(), 0.0f); std::fill (scDelay[c].begin(), scDelay[c].end(), 0.0f); }
        dryPos = 0;
        agc.reset();
        vizWrite.store (0);
    }

    void SpectralSuppressor::setParams (const Params& np)
    {
        if (np == p)
            return;
        const bool eqChanged = np.eq != p.eq || np.eqListen != p.eqListen;
        p = np;
        update (sampleRate);
        if (eqChanged)
            for (auto& f : listenFilter) f.reset();
    }

    void SpectralSuppressor::update (double sr)
    {
        orderFromIndex (p.order, orderArr);

        dyn.setSettings (p.dyn);
        dyn.setMonitorDetector (p.listen == static_cast<int> (Listen::Detector));
        ott.setSettings (p.ott);
        gate.setSettings (p.gate);

        for (int i = 0; i < kNumEqBands; ++i)
        {
            auto& band = eqBands[static_cast<size_t> (i)];
            const auto& nd = p.eq[static_cast<size_t> (i)];
            if (! nd.on) { band.count = 0; continue; }
            std::array<BiquadCoefs, 4> c;
            const int count = makeNodeCoefs (nd, sr, c);
            if (count != band.count)
                for (auto& ch : band.f) for (auto& f : ch) f.reset();
            band.count = count;
            for (int k = 0; k < count; ++k)
                for (auto& ch : band.f) ch[static_cast<size_t> (k)].set (c[static_cast<size_t> (k)]);
        }
        if (p.eqListen >= 1 && p.eqListen <= kNumEqBands)
        {
            FilterNode bp = p.eq[static_cast<size_t> (p.eqListen - 1)];
            bp.on = true; bp.type = static_cast<int> (FilterType::BandPass);
            bp.q = std::max (bp.q, 0.7f);
            std::array<BiquadCoefs, 4> c;
            makeNodeCoefs (bp, sr, c);
            for (auto& f : listenFilter) f.set (c[0]);
        }
    }

    bool SpectralSuppressor::isModuleEnabled (Module m) const
    {
        switch (m)
        {
            case Module::Suppressor: return p.dyn.enabled;
            case Module::Ott:        return p.ott.enabled;
            case Module::Gate:       return p.gate.enabled;
        }
        return false;
    }

    float SpectralSuppressor::moduleGainAt (Module m, int pt) const
    {
        switch (m)
        {
            case Module::Suppressor: return dyn.grAt (pt);
            case Module::Ott:        return ott.gainAt (pt);
            case Module::Gate:       return gate.gainAt (pt);
        }
        return 0.0f;
    }

    void SpectralSuppressor::process (const float* inL, const float* inR, const float* scL, const float* scR,
                                      float* outL, float* outR, int numSamples)
    {
        const float mix = p.mix, output = p.output;
        const int mask = static_cast<int> (dryDelay[0].size()) - 1;   // power of two
        const int lat[3] = { dyn.latency(), ott.latency(), gate.latency() };
        const int total = lat[0] + lat[1] + lat[2];
        const bool delta = p.listen == static_cast<int> (Listen::Delta);
        const bool bandListen = p.eqListen >= 1 && p.eqListen <= kNumEqBands;

        for (int s = 0; s < numSamples; ++s)
        {
            float x[2] = { inL[s], inR[s] };

            // Rings of the input and the sidechain: each stage reads its sidechain as far back
            // as the stages before it have delayed the audio; the dry path the whole way.
            dryDelay[0][static_cast<size_t> (dryPos)] = x[0];
            dryDelay[1][static_cast<size_t> (dryPos)] = x[1];
            scDelay[0][static_cast<size_t> (dryPos)] = scL ? scL[s] : 0.0f;
            scDelay[1][static_cast<size_t> (dryPos)] = scR ? scR[s] : 0.0f;
            const auto back = [&] (const std::vector<float>& ring, int d) { return ring[static_cast<size_t> ((dryPos - d) & mask)]; };
            float d[2] = { back (dryDelay[0], total), back (dryDelay[1], total) };

            int before = 0;
            for (int i = 0; i < kNumModules; ++i)
            {
                const int m = orderArr[i];
                // No external sidechain: each stage keys from its own input.
                const float sl = scL ? back (scDelay[0], before) : x[0];
                const float sr2 = scL ? back (scDelay[1], before) : x[1];
                if (m == static_cast<int> (Module::Suppressor)) dyn.process (x[0], x[1], sl, sr2);
                else if (m == static_cast<int> (Module::Ott))   ott.process (x[0], x[1], sl, sr2);
                else                                             gate.process (x[0], x[1], sl, sr2);
                before += lat[m];
            }
            dryPos = (dryPos + 1) & mask;

            // Manual EQ.
            for (int i = 0; i < kNumEqBands; ++i)
            {
                auto& band = eqBands[static_cast<size_t> (i)];
                if (band.count == 0) continue;
                const int chMode = p.eq[static_cast<size_t> (i)].channel;
                const auto run = [&band] (int c, float v)
                {
                    for (int k = 0; k < band.count; ++k) v = band.f[c][static_cast<size_t> (k)].process (v);
                    return v;
                };
                if (chMode == static_cast<int> (Channel::Mid) || chMode == static_cast<int> (Channel::Side))
                {
                    float mid = 0.5f * (x[0] + x[1]), side = 0.5f * (x[0] - x[1]);
                    if (chMode == static_cast<int> (Channel::Mid)) mid = run (0, mid); else side = run (0, side);
                    x[0] = mid + side; x[1] = mid - side;
                }
                else
                {
                    if (chMode != static_cast<int> (Channel::Right)) x[0] = run (0, x[0]);
                    if (chMode != static_cast<int> (Channel::Left))  x[1] = run (1, x[1]);
                }
            }
            if (bandListen)
                for (int ch = 0; ch < 2; ++ch) x[ch] = listenFilter[static_cast<size_t> (ch)].process (x[ch]);

            float makeup = 1.0f;
            if (p.autoGain && ! bandListen)
                makeup = agc.process (std::max (std::fabs (d[0]), std::fabs (d[1])), std::max (std::fabs (x[0]), std::fabs (x[1])));

            float o[2];
            for (int ch = 0; ch < 2; ++ch)
            {
                float v = d[ch] * (1.0f - mix) + x[ch] * makeup * mix;
                if (delta) v = d[ch] - v;
                v *= output;
                o[ch] = std::isfinite (v) ? v : 0.0f;
            }
            outL[s] = o[0]; outR[s] = o[1];

            const int w = vizWrite.load();
            vizInputBuf[static_cast<size_t> (w)] = 0.5f * (d[0] + d[1]);
            vizOutL[static_cast<size_t> (w)] = o[0];
            vizOutR[static_cast<size_t> (w)] = o[1];
            vizWrite.store ((w + 1) % kVizSize);
        }
    }

} // namespace temper
