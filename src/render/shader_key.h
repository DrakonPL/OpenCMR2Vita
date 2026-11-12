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
