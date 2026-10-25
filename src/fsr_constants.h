#pragma once
#include <cstdint>

// Fills the four uint4 constants consumed by FsrEasuF (con0..con3, 16 dwords).
void ComputeEasuConstants(uint32_t out[16], float inputW, float inputH, float outputW, float outputH);

// Fills the uint4 constant consumed by FsrRcasF. 0 = sharpest, each +1.0 halves the sharpening.
void ComputeRcasConstants(uint32_t out[4], float sharpnessStops);
