// SPDX-License-Identifier: MIT
#pragma once
#include <cstdlib>
#include <fftw3.h>

// Embedded deployments favor prompt startup over benchmarking every FFT plan.
inline unsigned w1700k_fft_plan_flags()
{
    const char *fast = std::getenv("W1700K_FFTW_ESTIMATE");
    return fast && fast[0] == '1' ? FFTW_ESTIMATE : FFTW_MEASURE;
}
