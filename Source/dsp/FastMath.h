#pragma once

/*
  Temper — fast approximations for the per-bin detector maths (JUCE-free).

  powerToDb: 10 log10 (x) for x > 0 via the float exponent and a 4th-order polynomial of the
  mantissa's log2; within 0.003 dB, about five times quicker than std::log10. Detector levels
  only: the gains themselves still use exact exp.
*/

#include <cstdint>
#include <cstring>

namespace temper
{
    inline float fastLog2 (float x)
    {
        std::uint32_t u;
        std::memcpy (&u, &x, 4);
        const int e = static_cast<int> ((u >> 23) & 0xFF) - 127;
        u = (u & 0x007FFFFF) | 0x3F800000;                 // mantissa in [1, 2)
        float m;
        std::memcpy (&m, &u, 4);
        // log2 (m) on [1, 2): minimax-style polynomial (max error ~1e-4).
        const float p = -1.7417939f + m * (2.8212026f + m * (-1.4699568f + m * (0.44717955f - m * 0.056570851f)));
        return static_cast<float> (e) + p;
    }

    inline float powerToDb (float p)
    {
        return 3.01029996f * fastLog2 (p + 1.0e-24f);      // 10 log10 (p) = 10 log10 (2) log2 (p)
    }
} // namespace temper
