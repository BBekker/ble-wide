# SPDX-License-Identifier: MIT
# FFTW may be outside the compiler's default search path (for example Homebrew).
FFTW_CFLAGS := $(shell pkg-config --cflags fftw3f 2>/dev/null)
FFTW_LIBS := $(shell pkg-config --libs fftw3f 2>/dev/null || echo -lfftw3f)
