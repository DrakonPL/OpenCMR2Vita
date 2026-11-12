// Keys of a draw's fragment program and pipeline, from its recorded state.
// The fragment key holds what selects code in render/shaders/fixed.frag
// (operations, texture kinds, alpha test, fog, specular, distance fade); the
// arguments and values stay uniforms. Shared by the executors, the frame
// trace (gfx_core.cpp) and the GXM shader variants.
#ifndef OPENCMR2_SHADER_KEY_H
#define OPENCMR2_SHADER_KEY_H

#include "render/gfx_internal.h"

#include <stdint.h>

// What a stage samples: 0 none, 1 2D, 2 cube, 3 bump, 4 distance field,
// 5 menu dot, 6 vector icon.
inline int Gfx_TextureKind(const GfxStageState &stage)
{
    GfxTexture *t = Gfx_DrawnTexture(stage.texture);
    if (t == NULL)
        return 0;
    if (t->flags & GFX_TEXTURE_CUBE)
        return 2;
    if (t->format == GFX_FORMAT_BUMP_DUDVL)
        return 3;
    if (t->flags & GFX_TEXTURE_DISTANCE_FIELD)
        return 4;
    if (t->flags & GFX_TEXTURE_MENU_DOT)
        return 5;
    if (t->flags & GFX_TEXTURE_VECTOR_ICON)
        return 6;
    return 1;
}

// A 2D texture drawn with point filtering and no mipmaps (the game's
// sprites, text and HUD). Scaled, nearest texels would come out one or two
// pixels wide, or be skipped; the shader filters them smoothly through a
// linear sampler. At their size it reads them as the point filter does.
inline bool Gfx_ScaledPoint(const GfxStageState &stage)
{
    int kind = Gfx_TextureKind(stage);
    return (kind == 1 || kind == 6) && stage.magFilter == GFX_FILTER_POINT && stage.mipFilter == GFX_MIPFILTER_NONE;
}

// 14 bits: colour op, alpha op (5 bits each), texture kind, scaled point.
inline uint64_t Gfx_StageKey(const GfxStageState &stage)
{
    if (stage.colorOp == GFX_TOP_DISABLE)
        return GFX_TOP_DISABLE;
    return (uint64_t)(stage.colorOp & 31) | (uint64_t)(stage.alphaOp & 31) << 5 |
           (uint64_t)(Gfx_TextureKind(stage) & 7) << 10 | (uint64_t)(Gfx_ScaledPoint(stage) ? 1 : 0) << 13;
}

// fade: the draw is a 3D one into the back buffer, which the distance fade
// applies to. Stage 1 counts only when stage 0 is on, as in the shader.
inline uint64_t Gfx_FragmentKey(const GfxDrawState &s, bool fade)
{
    const uint64_t stage0 = Gfx_StageKey(s.stages[0]);
    const uint64_t stage1 = s.stages[0].colorOp == GFX_TOP_DISABLE ? 0 : Gfx_StageKey(s.stages[1]);
    const bool fogTable = s.fog && s.fogTableMode == GFX_FOG_LINEAR;
    const bool distanceFade = fade && s.fadeEnd > s.fadeStart;
    return stage0 | stage1 << 14 | (uint64_t)(s.alphaTest ? 1 : 0) << 28 | (uint64_t)(s.fog ? 1 : 0) << 29 |
           (uint64_t)(fogTable ? 1 : 0) << 30 | (uint64_t)(s.specular ? 1 : 0) << 31 |
           (uint64_t)(distanceFade ? 1 : 0) << 32;
}

// ---- the arguments key ----
// What a fragment program reads from uniforms and a variant made for one
// draw state has as constants: the arguments of the texture stages and the
// form of the alpha test's compare. The GXM executor draws with the variant
// of a draw's arguments key where there is one (shaders/cg/variants.txt), and
// with the program that reads them from uniforms otherwise.

// One argument, 5 bits: its source (GFX_TA_DIFFUSE..GFX_TA_SPECULAR, any
// other reads as the current colour), complement, alpha replicate.
inline uint64_t Gfx_ArgKey(DWORD arg)
{
    const DWORD source = (arg & 0xf) <= GFX_TA_SPECULAR ? (arg & 0xf) : (DWORD)GFX_TA_CURRENT;
    return source | ((arg & GFX_TA_COMPLEMENT) ? 8u : 0u) | ((arg & GFX_TA_ALPHAREPLICATE) ? 16u : 0u);
}

// 20 bits: colour argument 1, 2, alpha argument 1, 2. An argument that the
// stage's operation does not read is 0, so that draws which differ only in
// it share a variant.
inline uint64_t Gfx_StageArgsKey(const GfxStageState &stage)
{
    const int op = stage.colorOp, alphaOp = stage.alphaOp;
    if (op == GFX_TOP_DISABLE || op == GFX_TOP_BUMPENVMAP || op == GFX_TOP_BUMPENVMAPLUMINANCE)
        return 0;
    uint64_t key = 0;
    if (op != GFX_TOP_SELECTARG2)
        key |= Gfx_ArgKey(stage.colorArg1);
    if (op != GFX_TOP_SELECTARG1)
        key |= Gfx_ArgKey(stage.colorArg2) << 5;
    if (alphaOp != GFX_TOP_DISABLE && alphaOp != GFX_TOP_SELECTARG2)
        key |= Gfx_ArgKey(stage.alphaArg1) << 10;
    if (alphaOp != GFX_TOP_DISABLE && alphaOp != GFX_TOP_SELECTARG1)
        key |= Gfx_ArgKey(stage.alphaArg2) << 15;
    return key;
}

// The alpha test as the fragment program runs it: 0 off, 1 alpha >=
// threshold, 2 alpha < threshold, 3 the general compare of the function and
// the reference.
enum { GFX_ALPHA_COMPARE_OFF, GFX_ALPHA_COMPARE_AT_LEAST, GFX_ALPHA_COMPARE_BELOW, GFX_ALPHA_COMPARE_GENERAL };

inline int Gfx_AlphaCompare(const GfxDrawState &s)
{
    if (!s.alphaTest)
        return GFX_ALPHA_COMPARE_OFF;
    switch (s.alphaFunc) {
    case GFX_CMP_GREATEREQUAL:
    case GFX_CMP_GREATER: return GFX_ALPHA_COMPARE_AT_LEAST;
    case GFX_CMP_LESS:
    case GFX_CMP_LESSEQUAL: return GFX_ALPHA_COMPARE_BELOW;
    default: return GFX_ALPHA_COMPARE_GENERAL;
    }
}

// The threshold of compare forms 1 and 2. Direct3D compares the alpha as a
// whole number of 255ths (rounded) with the reference: alpha * 255 + 0.5 >=
// ref is alpha >= (ref - 0.5) / 255, and > ref is >= ref + 1.
inline float Gfx_AlphaThreshold(const GfxDrawState &s)
{
    const float ref = (float)(s.alphaRef & 0xff);
    const bool above = s.alphaFunc == GFX_CMP_GREATER || s.alphaFunc == GFX_CMP_LESSEQUAL;
    return (ref + (above ? 0.5f : -0.5f)) / 255.0f;
}

// Stage 0 in bits 0-19, stage 1 in 20-39 (when it runs), the compare form
// in 40-41.
inline uint64_t Gfx_FragmentArgsKey(const GfxDrawState &s)
{
    const bool stage1 = s.stages[0].colorOp != GFX_TOP_DISABLE && s.stages[1].colorOp != GFX_TOP_DISABLE;
    return Gfx_StageArgsKey(s.stages[0]) | (stage1 ? Gfx_StageArgsKey(s.stages[1]) << 20 : 0) |
           (uint64_t)Gfx_AlphaCompare(s) << 40;
}

// Everything of a draw's pipeline that the state can change: vertex format,
// primitive, blending, depth and culling. primitive is a list or strip type
// (fans are converted before they are drawn).
inline uint32_t Gfx_PipelineKey(int primitive, bool transformed, const GfxDrawState &s)
{
    bool depthTest = s.depthTest;
    return (transformed ? 1u : 0u) | (uint32_t)(primitive & 7) << 1 | (s.alphaBlend ? 1u : 0u) << 4 |
           (uint32_t)(s.srcBlend & 15) << 5 | (uint32_t)(s.destBlend & 15) << 9 | (depthTest ? 1u : 0u) << 13 |
           ((depthTest && s.depthWrite) ? 1u : 0u) << 14 | (uint32_t)(s.depthFunc & 15) << 15 |
           (uint32_t)(s.cull & 3) << 19;
}

#endif
