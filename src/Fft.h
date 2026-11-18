#pragma once
#include <complex>
#include <vector>

namespace arraylab
{
/** In-place radix-2 complex FFT. Size must be a power of two.
    The inverse transform is scaled by 1/N. */
class Fft
{
public:
    explicit Fft (int size);
    int size() const noexcept { return n; }
    void forward (std::complex<float>* data) const noexcept { transform (data, false); }
    void inverse (std::complex<float>* data) const noexcept { transform (data, true); }

private:
    void transform (std::complex<float>* data, bool inverse) const noexcept;
    int n;
    std::vector<int> bitrev;
    std::vector<std::complex<float>> twiddle; // exp(-2*pi*i*k/N), k < N/2
};
}
