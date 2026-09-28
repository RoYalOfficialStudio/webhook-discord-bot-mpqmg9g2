#include "core/Math.h"

#if defined(__SSE__) || defined(_M_X64) || defined(_M_IX86)
#include <xmmintrin.h>
#define ROY_HAS_SSE 1
#endif

namespace roy {

ScopedNoDenormals::ScopedNoDenormals() {
#ifdef ROY_HAS_SSE
    previous_ = _mm_getcsr();
    _mm_setcsr(previous_ | 0x8040); // FTZ | DAZ
#endif
}

ScopedNoDenormals::~ScopedNoDenormals() {
#ifdef ROY_HAS_SSE
    _mm_setcsr(previous_);
#endif
}

} // namespace roy
