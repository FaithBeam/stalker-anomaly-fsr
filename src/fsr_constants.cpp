// CPU-side constant setup, using AMD's reference implementation from ffx_fsr1.h.
#include <cstdint>
#include <cmath>
#include <cstdlib> // labs(), used by ffx_a.h on non-GCC compilers (MSVC)

#define A_CPU 1
#include "ffx_a.h"
#include "ffx_fsr1.h"

#include "fsr_constants.h"

void ComputeEasuConstants(uint32_t out[16], float inputW, float inputH, float outputW, float outputH)
{
    FsrEasuCon(out, out + 4, out + 8, out + 12, inputW, inputH, inputW, inputH, outputW, outputH);
}

void ComputeRcasConstants(uint32_t out[4], float sharpnessStops)
{
    FsrRcasCon(out, sharpnessStops);
}
