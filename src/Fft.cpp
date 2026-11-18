#include "Fft.h"
#include <cmath>
#include <utility>

namespace arraylab
{
Fft::Fft (int size) : n (size), bitrev ((size_t) size), twiddle ((size_t) size / 2)
{
    int bits = 0;
    while ((1 << bits) < n) ++bits;

    for (int i = 0; i < n; ++i)
    {
        int r = 0;
        for (int b = 0; b < bits; ++b)
            if (i & (1 << b)) r |= 1 << (bits - 1 - b);
        bitrev[(size_t) i] = r;
    }

    const double pi = 3.14159265358979323846;
    for (int k = 0; k < n / 2; ++k)
        twiddle[(size_t) k] = { (float) std::cos (-2.0 * pi * k / n), (float) std::sin (-2.0 * pi * k / n) };
}

void Fft::transform (std::complex<float>* d, bool inv) const noexcept
{
    for (int i = 0; i < n; ++i)
    {
        const int j = bitrev[(size_t) i];
        if (j > i) std::swap (d[i], d[j]);
    }

    for (int len = 2; len <= n; len <<= 1)
    {
        const int half = len / 2, step = n / len;
        for (int i = 0; i < n; i += len)
            for (int k = 0; k < half; ++k)
            {
                auto w = twiddle[(size_t) (k * step)];
                if (inv) w = std::conj (w);
                const auto t = d[i + k + half] * w;
                d[i + k + half] = d[i + k] - t;
                d[i + k] += t;
            }
    }

    if (inv)
    {
        const float s = 1.0f / (float) n;
        for (int i = 0; i < n; ++i) d[i] *= s;
    }
}
}
