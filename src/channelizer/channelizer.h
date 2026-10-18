// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <cmath>
#include <cstring>
#ifdef W1700K_PORTABLE_FFT
#include "channelizer/fft80.h"
using fftwf_complex = float[2];
#else
#include <fftw3.h>
#endif
#include <stdexcept>

// 160 Msps -> eighty 2 Msps bins. A 1 MHz input translation puts every
// BLE center on a bin with LO=2441 MHz. Only the forty BLE bins are decoded.
class Channelizer {
    static constexpr int N = 80, T = 16;
    alignas(64) float history[T][N * 2]{}, coefficients[T][N * 2]{};
    float oscillator[160][2]{};
#ifdef W1700K_PORTABLE_FFT
    alignas(64) fftwf_complex input[80]{}, output[80]{};
    FFT80 fft;
#else
    fftwf_complex *input, *output;
    fftwf_plan plan;
#endif
    int head = 0, position = 0, phase = 0;

  public:
    Channelizer() {
#ifndef W1700K_PORTABLE_FFT
        input = fftwf_alloc_complex(N);
        output = fftwf_alloc_complex(N);
        if (!input || !output)
            throw std::runtime_error("FFT allocation failed");
        plan = fftwf_plan_dft_1d(N, input, output, FFTW_BACKWARD, FFTW_MEASURE);
        if (!plan)
            throw std::runtime_error("FFT plan failed");
#endif
        constexpr double pi = 3.14159265358979323846;
        double h[N * T], sum = 0;
        for (int k = 0; k < N * T; k++) {
            double x = k - (N * T - 1) / 2.0, a = 2 * pi * k / (N * T - 1);
            double v = (x == 0 ? 2 * .65 / 160 : sin(2 * pi * .65 / 160 * x) / (pi * x));
            h[k] = v * (.42 - .5 * cos(a) + .08 * cos(2 * a));
            sum += h[k];
        }
        for (int t = 0; t < T; t++)
            for (int p = 0; p < N; p++)
                coefficients[t][2 * p] = coefficients[t][2 * p + 1] = float(h[t * N + p] / sum);
        for (int k = 0; k < 160; k++) {
            oscillator[k][0] = float(cos(-2 * pi * k / 160));
            oscillator[k][1] = float(sin(-2 * pi * k / 160));
        }
    }
    ~Channelizer() {
#ifndef W1700K_PORTABLE_FFT
        fftwf_destroy_plan(plan);
        fftwf_free(input);
        fftwf_free(output);
#endif
    }
    Channelizer(const Channelizer &) = delete;
    void reset() {
        std::memset(history, 0, sizeof(history));
        head = position = phase = 0;
    }
    template <class F> void run(const int16_t *iq, size_t n, F emit) {
        for (size_t k = 0; k < n; k++) {
            float i = iq[2 * k], q = iq[2 * k + 1], c = oscillator[phase][0],
                  s = oscillator[phase][1];
            history[head][2 * (N - 1 - position)] = i * c - q * s;
            history[head][2 * (N - 1 - position) + 1] = i * s + q * c;
            phase = (phase + 1) % 160;
            if (++position != N)
                continue;
            position = 0;
            float *v = &input[0][0];
            std::fill(v, v + 2 * N, 0.0f);
            for (int t = 0; t < T; t++) {
                const float *x = history[(head - t + T) % T], *h = coefficients[t];
                for (int p = 0; p < 2 * N; p++)
                    v[p] += x[p] * h[p];
            }
#ifdef W1700K_PORTABLE_FFT
            fft.run(input, output);
#else
            fftwf_execute(plan);
#endif
            emit(output);
            head = (head + 1) % T;
        }
    }
    static int mhz(int ch) {
        return ch == 37   ? 2402
               : ch == 38 ? 2426
               : ch == 39 ? 2480
               : ch < 11  ? 2404 + 2 * ch
                          : 2406 + 2 * ch;
    }
    static int bin(int ch) { return ((mhz(ch) - 2442) / 2 + 80) % 80; }
};
