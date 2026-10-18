// SPDX-License-Identifier: MIT
#pragma once
#include <cmath>

// Positive-sign, unnormalised DFT, matching FFTW_BACKWARD. 80 = 5 * 16.
// Kept independent of FFTW so the browser build has no external FFT runtime.
class FFT80 {
    struct C {
        float r, i;
        C operator+(C b) const { return {r + b.r, i + b.i}; }
        C operator-(C b) const { return {r - b.r, i - b.i}; }
        C operator*(C b) const { return {r * b.r - i * b.i, r * b.i + i * b.r}; }
        C operator*(float b) const { return {r * b, i * b}; }
    };
    C twiddle[5][16], roots[8];
  public:
    FFT80() {
        const double pi = std::acos(-1.0);
        for (int r = 0; r < 5; r++)
            for (int q = 0; q < 16; q++) {
                double a = 2 * pi * r * q / 80;
                twiddle[r][q] = {float(std::cos(a)), float(std::sin(a))};
            }
        for (int k = 0; k < 8; k++) {
            double a = 2 * pi * k / 16;
            roots[k] = {float(std::cos(a)), float(std::sin(a))};
        }
    }
    void run(const float (*in)[2], float (*out)[2]) const {
        C a[5][16];
        for (int r = 0; r < 5; r++) {
            for (unsigned s = 0; s < 16; s++) {
                unsigned b = ((s & 1) << 3) | ((s & 2) << 1) | ((s & 4) >> 1) | ((s & 8) >> 3);
                a[r][b] = {in[r + 5 * s][0], in[r + 5 * s][1]};
            }
            for (int n = 2; n <= 16; n *= 2)
                for (int start = 0; start < 16; start += n)
                    for (int j = 0; j < n / 2; j++) {
                        C x = a[r][start + j], y = a[r][start + j + n / 2] * roots[j * 16 / n];
                        a[r][start + j] = x + y;
                        a[r][start + j + n / 2] = x - y;
                    }
        }
        constexpr float c1 = .30901699437494745f, c2 = -.80901699437494745f;
        constexpr float s1 = .9510565162951535f, s2 = .5877852522924731f;
        for (int q = 0; q < 16; q++) {
            C x[5];
            for (int r = 0; r < 5; r++) x[r] = a[r][q] * twiddle[r][q];
            C t1 = x[1] + x[4], t2 = x[2] + x[3], t3 = x[1] - x[4], t4 = x[2] - x[3];
            C v1 = x[0] + t1 * c1 + t2 * c2, v2 = x[0] + t1 * c2 + t2 * c1;
            C y1 = t3 * s1 + t4 * s2, y2 = t3 * s2 - t4 * s1;
            C y[5] = {x[0] + t1 + t2, v1 + C{-y1.i, y1.r},
                      v2 + C{-y2.i, y2.r}, v2 - C{-y2.i, y2.r}, v1 - C{-y1.i, y1.r}};
            for (int p = 0; p < 5; p++) {
                out[q + 16 * p][0] = y[p].r;
                out[q + 16 * p][1] = y[p].i;
            }
        }
    }
};
