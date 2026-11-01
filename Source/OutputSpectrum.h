#pragma once

#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <cmath>
#include <vector>

// Spectrum of the plugin's output for the analyzer display (message thread only).
// Takes the louder of L and R per FFT bin, so wide or out-of-phase content that a mono
// sum would cancel still shows. Levels are dBFS: a full-scale sine reads 0 dB.
class OutputSpectrum
{
public:
    static constexpr int   kPoints  = 256;       // log-spaced display points
    static constexpr float kFloorDb = -90.0f;
    static constexpr float kLowHz   = 20.0f;
    static constexpr float kHighHz  = 20000.0f;

    OutputSpectrum()
    {
        window.resize (kFftSize);
        for (int i = 0; i < kFftSize; ++i)
            window[static_cast<size_t> (i)] = 0.5f * (1.0f - std::cos (2.0f * juce::MathConstants<float>::pi
                                                                       * static_cast<float> (i) / static_cast<float> (kFftSize)));
        fftBuf.assign (kFftSize * 2, 0.0f);
        peak.assign (kFftSize / 2 + 1, 0.0f);
        db.assign (kPoints, kFloorDb);
    }

    static float freqAt (int point)
    {
        return kLowHz * std::pow (kHighHz / kLowHz, static_cast<float> (point) / static_cast<float> (kPoints - 1));
    }

    float dbAt (int point) const { return db[static_cast<size_t> (point)]; }

    // ringL/ringR are the engine's output ring buffers; writePos is the next write index.
    void update (const std::vector<float>& ringL, const std::vector<float>& ringR, int writePos, double sampleRate)
    {
        if (ringL.size() < static_cast<size_t> (kFftSize) || ringR.size() != ringL.size() || sampleRate <= 0.0)
            return;

        std::fill (peak.begin(), peak.end(), 0.0f);
        accumulate (ringL, writePos);
        accumulate (ringR, writePos);

        const float binsPerHz = static_cast<float> (kFftSize) / static_cast<float> (sampleRate);
        const float halfStep  = std::pow (kHighHz / kLowHz, 0.5f / static_cast<float> (kPoints - 1));
        const float scale     = 4.0f / static_cast<float> (kFftSize);   // Hann coherent gain + one-sided

        for (int i = 0; i < kPoints; ++i)
        {
            const float f = freqAt (i);
            const float centre = f * binsPerHz;
            const float lo = centre / halfStep, hi = centre * halfStep;
            float mag = 0.0f;

            if (hi >= static_cast<float> (kFftSize / 2))
                mag = 0.0f;                                 // above Nyquist at this sample rate
            else if (hi - lo < 1.0f)
            {
                // Narrower than an FFT bin (low end): interpolate between neighbours.
                const int k = static_cast<int> (centre);
                const float t = centre - static_cast<float> (k);
                mag = peak[static_cast<size_t> (k)] + t * (peak[static_cast<size_t> (k + 1)] - peak[static_cast<size_t> (k)]);
            }
            else
            {
                // Wider than a bin (high end): keep the strongest so peaks aren't lost.
                for (int k = static_cast<int> (std::ceil (lo)); k <= static_cast<int> (hi); ++k)
                    mag = std::max (mag, peak[static_cast<size_t> (k)]);
            }

            const float target = std::max (kFloorDb, 20.0f * std::log10 (std::max (mag * scale, 1.0e-9f)));
            float& d = db[static_cast<size_t> (i)];
            d += (target > d ? kRise : kFall) * (target - d);
        }
    }

private:
    static constexpr int   kOrder   = 12;
    static constexpr int   kFftSize = 1 << kOrder;   // 4096
    static constexpr float kRise    = 0.7f;          // per update: quick to show new energy
    static constexpr float kFall    = 0.25f;         // ...slower to let it fall, so it reads steadily

    void accumulate (const std::vector<float>& ring, int writePos)
    {
        const int n = static_cast<int> (ring.size());
        std::fill (fftBuf.begin(), fftBuf.end(), 0.0f);
        for (int i = 0; i < kFftSize; ++i)
        {
            const int idx = (((writePos - kFftSize + i) % n) + n) % n;
            fftBuf[static_cast<size_t> (i)] = ring[static_cast<size_t> (idx)] * window[static_cast<size_t> (i)];
        }
        fft.performFrequencyOnlyForwardTransform (fftBuf.data(), true);
        for (size_t k = 0; k < peak.size(); ++k)
            peak[k] = std::max (peak[k], fftBuf[k]);
    }

    juce::dsp::FFT fft { kOrder };
    std::vector<float> window, fftBuf, peak, db;
};
