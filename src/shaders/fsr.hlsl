// Fullscreen passes for AMD FidelityFX Super Resolution 1.0.
//   FSR_PASS_EASU: Edge Adaptive Spatial Upsampling (game resolution -> display resolution)
//   FSR_PASS_RCAS: Robust Contrast Adaptive Sharpening (display resolution, in place)
// Compiled at runtime by fsr_pass.cpp; ffx_a.h / ffx_fsr1.h are AMD's unmodified headers.

#define A_GPU 1
#define A_HLSL 1
#include "ffx_a.h"

Texture2D<float4> InputTexture : register(t0);
SamplerState LinearClamp : register(s0);

cbuffer FsrConstants : register(b0)
{
    uint4 Const0;
    uint4 Const1;
    uint4 Const2;
    uint4 Const3;
};

// One oversized triangle covering the viewport; no vertex buffer needed.
float4 FullscreenVS(uint id : SV_VertexID) : SV_Position
{
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

#if defined(FSR_PASS_EASU)

#define FSR_EASU_F 1
AF4 FsrEasuRF(AF2 p) { return InputTexture.GatherRed(LinearClamp, p); }
AF4 FsrEasuGF(AF2 p) { return InputTexture.GatherGreen(LinearClamp, p); }
AF4 FsrEasuBF(AF2 p) { return InputTexture.GatherBlue(LinearClamp, p); }
#include "ffx_fsr1.h"

float4 EasuPS(float4 pos : SV_Position) : SV_Target
{
    AF3 color;
    FsrEasuF(color, AU2(pos.xy), Const0, Const1, Const2, Const3);
    return float4(color, 1.0);
}

#elif defined(FSR_PASS_RCAS)

#define FSR_RCAS_F 1
AF4 FsrRcasLoadF(ASU2 p) { return InputTexture.Load(int3(p, 0)); }
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}
#include "ffx_fsr1.h"

float4 RcasPS(float4 pos : SV_Position) : SV_Target
{
    AF3 color;
    FsrRcasF(color.r, color.g, color.b, AU2(pos.xy), Const0);
    return float4(color, 1.0);
}

#endif
