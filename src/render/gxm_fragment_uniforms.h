// The rows of fu[] in the fixed_f variants (shaders/cg/uniforms.h) from a
// recorded draw state, for the GXM executor (gfx_gxm.cpp). The executor keeps
// the rows it gave the GPU and gives them again only when they change. Thus
// the rows hold nothing that the draw's program does not read: most draws of
// a stage differ from the draw before only in their textures. No GXM:
// tests/gxm_fragment_uniforms.cpp.
#ifndef OPENCMR2_GXM_FRAGMENT_UNIFORMS_H
#define OPENCMR2_GXM_FRAGMENT_UNIFORMS_H

#include "render/gfx_executor_shared.h"
#include "render/shader_key.h"
#include "render/shaders/cg/uniforms.h"

// A frame with no backdrop copy does not fade: the range starts beyond any eye
// distance. The start and the end are different: smoothstep with equal edges
// divides by zero, and on the device each faded pixel then showed the
// backdrop.
constexpr float kGxmNoFadeStart = 1e18f;
constexpr float kGxmNoFadeEnd = 2e18f;

// Whether the program of a stage reads the size of its texture (fixed_f.cg.in:
// a menu dot, a vector icon, and a point-filtered texture that is scaled).
inline bool Gxm_ReadsTextureSize(const GfxStageState &stage)
{
    const int kind = Gfx_TextureKind(stage);
    return kind == 5 || kind == 6 || Gfx_ScaledPoint(stage);
}

// fade: the frame has its backdrop copy.
inline void Gxm_FillFragmentUniforms(const GfxDrawState &s, int targetW, int targetH, bool fade, float u[FU_ROWS][4])
{
    memset(u, 0, sizeof(float) * 4 * FU_ROWS);
    for (int i = 0; i < 2; i++) {
        const GfxStageState &stage = s.stages[i];
        const int first = i == 0 ? FU_STAGE0_SOURCE : FU_STAGE1_SOURCE;
        const int args[4] = { stage.colorArg1, stage.colorArg2, stage.alphaArg1, stage.alphaArg2 };
        for (int k = 0; k < 4; k++) {
            const int source = args[k] & 0xf;
            u[first][k] = (float)(source <= GFX_TA_SPECULAR ? source : GFX_TA_CURRENT);
            u[first + 1][k] = (args[k] & GFX_TA_COMPLEMENT) ? 1.0f : 0.0f;
            u[first + 2][k] = (args[k] & GFX_TA_ALPHAREPLICATE) ? 1.0f : 0.0f;
        }
        // The texture drawn, and the original a vector icon also reads.
        const GfxTexture *drawn = Gfx_DrawnTexture(stage.texture);
        float *size = u[i == 0 ? FU_TEX_SIZE0 : FU_TEX_SIZE1];
        if (drawn != nullptr && Gxm_ReadsTextureSize(stage)) {
            size[0] = (float)drawn->width;
            size[1] = (float)drawn->height;
            size[2] = (float)stage.texture->width;
            size[3] = (float)stage.texture->height;
        }
    }
    GfxShared::Color(s.textureFactor, u[FU_TEXTURE_FACTOR]);
    u[FU_ALPHA_TEST][0] = (float)s.alphaFunc;
    u[FU_ALPHA_TEST][1] = (float)(s.alphaRef & 0xff);
    u[FU_ALPHA_TEST][2] = Gfx_AlphaThreshold(s);
    GfxShared::Color(s.fogColor, u[FU_FOG_COLOR]);
    u[FU_FOG_PARAMS][0] = s.fogStart;
    u[FU_FOG_PARAMS][1] = s.fogEnd;
    memcpy(u[FU_BUMP_MATRIX], s.stages[0].bumpMat, sizeof(float) * 4);
    u[FU_BUMP_LUMINANCE][0] = s.stages[0].bumpLumScale;
    u[FU_BUMP_LUMINANCE][1] = s.stages[0].bumpLumOffset;
    u[FU_DISTANCE_FADE][0] = fade ? s.fadeStart : kGxmNoFadeStart;
    u[FU_DISTANCE_FADE][1] = fade ? s.fadeEnd : kGxmNoFadeEnd;
    u[FU_TARGET][0] = targetW ? 1.0f / targetW : 0.0f;
    u[FU_TARGET][1] = targetH ? 1.0f / targetH : 0.0f;
}

#endif
