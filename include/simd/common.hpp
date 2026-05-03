/*
 * DISTRHO Cardinal Plugin
 * Copyright (C) 2021-2024 Filipe Coelho <falktx@falktx.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#define CARDINAL_INCLUDING_EMULATED_IMMINTRIN_H
#include "mmintrin.h"
#include "xmmintrin.h"
#include "emmintrin.h"
#include "pmmintrin.h"
#include "tmmintrin.h"
#include "smmintrin.h"
#undef CARDINAL_INCLUDING_EMULATED_IMMINTRIN_H

// MinGW 13 defines these via __attribute__((target)) in <immintrin.h>,
// conflicting with simde aliases. Undef them to avoid redefinition errors.
#undef _mm_loadu_epi8
#undef _mm_loadu_epi16
#undef _mm_loadu_epi32
#undef _mm_loadu_epi64
