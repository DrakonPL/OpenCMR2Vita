// Helpers that turn recorded state into uniforms and rectangles, shared by
// the executors (gfx_gpu.cpp, gfx_gl.cpp, gfx_gxm.cpp).
#ifndef OPENCMR2_GFX_EXECUTOR_SHARED_H
#define OPENCMR2_GFX_EXECUTOR_SHARED_H

#include "render/gfx_internal.h"

#include <math.h>
#include <string.h>

#include <algorithm>

namespace GfxShared {

inline void Multiply(const GfxMatrix &a, const GfxMatrix &b, float *out)
{
    const float *pa = &a._11, *pb = &b._11;
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            float s = 0;
            for (int k = 0; k < 4; k++)
                s += pa[r * 4 + k] * pb[k * 4 + c];
            out[r * 4 + c] = s;
        }
}

inline void TransformPoint(const GfxVector &v, float w, const GfxMatrix &m, float out[3])
{
    const float *p = &m._11;
    for (int c = 0; c < 3; c++)
        out[c] = v.x * p[c] + v.y * p[4 + c] + v.z * p[8 + c] + w * p[12 + c];
}

inline void Color(DWORD c, float out[4])
{
    out[0] = ((c >> 16) & 0xff) / 255.0f;
    out[1] = ((c >> 8) & 0xff) / 255.0f;
    out[2] = (c & 0xff) / 255.0f;
    out[3] = ((c >> 24) & 0xff) / 255.0f;
}

inline void Copy4(const GfxColorValue &c, float out[4])
{
    out[0] = c.r;
    out[1] = c.g;
    out[2] = c.b;
    out[3] = c.a;
}

// Whether two states give the same vertex uniforms.
inline bool SameVertexUniforms(const GfxDrawState &a, const GfxDrawState &b)
{
    for (int i = 0; i < 2; i++)
        if (a.stages[i].texCoordIndex != b.stages[i].texCoordIndex ||
            a.stages[i].textureTransform != b.stages[i].textureTransform)
            return false;
    return a.viewport.width == b.viewport.width && a.viewport.height == b.viewport.height &&
           a.screenW == b.screenW && a.screenH == b.screenH && a.lighting == b.lighting && a.colorVertex == b.colorVertex && a.specular == b.specular &&
           a.localViewer == b.localViewer && a.normalizeNormals == b.normalizeNormals &&
           a.texGen1 == b.texGen1 && memcmp(a.texGen1Rows, b.texGen1Rows, sizeof(a.texGen1Rows)) == 0 &&
           a.diffuseSource == b.diffuseSource && a.specularSource == b.specularSource &&
           a.ambientSource == b.ambientSource && a.emissiveSource == b.emissiveSource && a.fog == b.fog &&
           a.fogTableMode == b.fogTableMode && a.fogVertexMode == b.fogVertexMode &&
           a.fogStart == b.fogStart && a.fogEnd == b.fogEnd && a.ambient == b.ambient &&
           memcmp(&a.world, &b.world, sizeof(a.world)) == 0 && memcmp(&a.view, &b.view, sizeof(a.view)) == 0 &&
           memcmp(&a.projection, &b.projection, sizeof(a.projection)) == 0 &&
           memcmp(a.texture, b.texture, sizeof(a.texture)) == 0 &&
           memcmp(&a.material, &b.material, sizeof(a.material)) == 0 &&
           memcmp(a.lightEnabled, b.lightEnabled, sizeof(a.lightEnabled)) == 0 &&
           memcmp(a.lights, b.lights, sizeof(a.lights)) == 0;
}

// A rectangle of the screen the game draws in (Gfx_SetVirtualScreen) in
// the back buffer's pixels, within it; elsewhere as it is.
inline GfxViewport ToTarget(const GfxViewport &v, int screenW, int screenH, bool backBuffer, int targetW,
                            int targetH)
{
    if (!backBuffer || screenW <= 0 || screenH <= 0)
        return v;
    float kx = (float)targetW / screenW, ky = (float)targetH / screenH;
    int x0 = std::clamp((int)lroundf(v.x * kx), 0, targetW);
    int y0 = std::clamp((int)lroundf(v.y * ky), 0, targetH);
    int x1 = std::clamp((int)lroundf((v.x + v.width) * kx), x0, targetW);
    int y1 = std::clamp((int)lroundf((v.y + v.height) * ky), y0, targetH);
    GfxViewport r = v;
    r.x = x0;
    r.y = y0;
    r.width = x1 - x0;
    r.height = y1 - y0;
    return r;
}

} // namespace GfxShared

#endif
