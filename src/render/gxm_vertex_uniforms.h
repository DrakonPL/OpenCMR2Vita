// The rows of vu[] in fixed3d_v.cg (shaders/cg/uniforms.h) from a recorded
// draw state. The GXM executor (gfx_gxm.cpp) fills them once and then
// changes only the rows that differ from the draw before: a full fill for
// each draw was the largest step of a frame on the device. No GXM:
// tests/gxm_vertex_uniforms.cpp checks the update against the fill.
#ifndef OPENCMR2_GXM_VERTEX_UNIFORMS_H
#define OPENCMR2_GXM_VERTEX_UNIFORMS_H

#include "render/gfx_executor_shared.h"
#include "render/shaders/cg/uniforms.h"

// The source a stage's texture coordinate index names (VU_TEX_COORD).
inline float Gxm_TexCoordSource(DWORD index)
{
    if ((index & 0xffff0000) == 0x30000)
        return 2.0f;    // camera-space reflection vector
    return (index & 0xffff) == 1 ? 1.0f : 0.0f;
}

// The material rows.
inline void Gxm_MaterialRows(const GfxDrawState &s, float u[VU_ROWS][4])
{
    GfxShared::Copy4(s.material.diffuse, u[VU_MATERIAL_DIFFUSE]);
    GfxShared::Copy4(s.material.ambient, u[VU_MATERIAL_AMBIENT]);
    GfxShared::Copy4(s.material.specular, u[VU_MATERIAL_SPECULAR]);
    GfxShared::Copy4(s.material.emissive, u[VU_MATERIAL_EMISSIVE]);
}

// The rows of the eight lights, in eye space. The rows of a light that is
// off are 0: fixed3d_v.cg skips it by its enabled flag.
inline void Gxm_LightRows(const GfxDrawState &s, float u[VU_ROWS][4])
{
    memset(u[VU_LIGHTS], 0, sizeof(float) * 4 * 8 * VU_LIGHT_ROWS);
    for (int i = 0; i < 8; i++) {
        if (!s.lightEnabled[i])
            continue;
        const GfxLight &l = s.lights[i];
        float(*row)[4] = &u[VU_LIGHTS + i * VU_LIGHT_ROWS];
        GfxShared::Copy4(l.diffuse, row[VU_LIGHT_DIFFUSE]);
        GfxShared::Copy4(l.specular, row[VU_LIGHT_SPECULAR]);
        GfxShared::Copy4(l.ambient, row[VU_LIGHT_AMBIENT]);
        GfxShared::TransformPoint(l.position, 1.0f, s.view, row[VU_LIGHT_POSITION]);
        row[VU_LIGHT_POSITION][3] = (float)l.type;
        GfxShared::TransformPoint(l.direction, 0.0f, s.view, row[VU_LIGHT_DIRECTION]);
        row[VU_LIGHT_DIRECTION][3] = l.range > 0.0f ? l.range : 1e30f;
        row[VU_LIGHT_ATTENUATION][0] = l.attenuation0;
        row[VU_LIGHT_ATTENUATION][1] = l.attenuation1;
        row[VU_LIGHT_ATTENUATION][2] = l.attenuation2;
        row[VU_LIGHT_ATTENUATION][3] = l.falloff;
        row[VU_LIGHT_SPOT][0] = cosf(l.theta * 0.5f);
        row[VU_LIGHT_SPOT][1] = cosf(l.phi * 0.5f);
        row[VU_LIGHT_SPOT][2] = 1.0f;
    }
}

// The rows that are a few values each: written for each draw.
inline void Gxm_SmallRows(const GfxDrawState &s, const GfxViewport &viewport, float u[VU_ROWS][4])
{
    GfxShared::Color(s.ambient, u[VU_GLOBAL_AMBIENT]);
    u[VU_GLOBAL_AMBIENT][3] = s.material.power;
    u[VU_LIGHT_FLAGS][0] = s.lighting;
    u[VU_LIGHT_FLAGS][1] = s.colorVertex;
    u[VU_LIGHT_FLAGS][2] = s.specular;
    u[VU_LIGHT_FLAGS][3] = s.localViewer;
    u[VU_SOURCES][0] = (float)s.diffuseSource;
    u[VU_SOURCES][1] = (float)s.specularSource;
    u[VU_SOURCES][2] = (float)s.ambientSource;
    u[VU_SOURCES][3] = (float)s.emissiveSource;
    const bool vertexFog = s.fog && s.fogTableMode == GFX_FOG_NONE;
    u[VU_FOG_FLAGS][0] = vertexFog && s.fogVertexMode == GFX_FOG_LINEAR;
    u[VU_FOG_FLAGS][1] = s.normalizeNormals;
    // Direct3D takes the fog factor from the vertex's specular alpha when
    // fog is on with neither vertex nor table fog (the backdrop of a foggy stage).
    u[VU_FOG_FLAGS][2] = vertexFog && s.fogVertexMode == GFX_FOG_NONE;
    u[VU_FOG_FLAGS][3] = 0.0f;
    u[VU_FOG_PARAMS][0] = s.fogStart;
    u[VU_FOG_PARAMS][1] = s.fogEnd;
    u[VU_FOG_PARAMS][2] = u[VU_FOG_PARAMS][3] = 0.0f;
    u[VU_TEX_COORD][0] = Gxm_TexCoordSource(s.stages[0].texCoordIndex);
    u[VU_TEX_COORD][1] = Gxm_TexCoordSource(s.stages[1].texCoordIndex);
    u[VU_TEX_COORD][2] = s.stages[0].textureTransform != GFX_TTFF_DISABLE;
    u[VU_TEX_COORD][3] = s.stages[1].textureTransform != GFX_TTFF_DISABLE;
    // Direct3D 7's half-pixel offset of the viewport transform.
    u[VU_TARGET][0] = u[VU_TARGET][1] = 0.0f;
    u[VU_TARGET][2] = viewport.width ? 1.0f / viewport.width : 0.0f;
    u[VU_TARGET][3] = viewport.height ? -1.0f / viewport.height : 0.0f;
}

// All rows of s. Without lighting the light rows are 0.
inline void Gxm_FillVertexUniforms(const GfxDrawState &s, const GfxViewport &viewport, float u[VU_ROWS][4])
{
    GfxShared::Multiply(s.world, s.view, u[VU_WORLD_VIEW]);
    memcpy(u[VU_PROJECTION], &s.projection, 64);
    memcpy(u[VU_TEXTURE_MATRIX0], &s.texture[0], 64);
    memcpy(u[VU_TEXTURE_MATRIX1], &s.texture[1], 64);
    Gxm_MaterialRows(s, u);
    if (s.lighting)
        Gxm_LightRows(s, u);
    else
        memset(u[VU_LIGHTS], 0, sizeof(float) * 4 * 8 * VU_LIGHT_ROWS);
    Gxm_SmallRows(s, viewport, u);
}

// The rows of s where u holds those of prev: only the rows that differ are
// written. With lighting off in s the light rows are not made current: the
// vertex program does not read them then, and the executor does not upload
// them (VU_UNLIT_ROWS). The next state with lighting makes them current.
// The rows up to VU_UNLIT_ROWS, and with lighting all rows, are those that
// Gxm_FillVertexUniforms gives.
inline void Gxm_UpdateVertexUniforms(const GfxDrawState &prev, const GfxDrawState &s, const GfxViewport &viewport,
                                     float u[VU_ROWS][4])
{
    const bool view = memcmp(&prev.view, &s.view, sizeof(s.view)) != 0;
    if (view || memcmp(&prev.world, &s.world, sizeof(s.world)) != 0)
        GfxShared::Multiply(s.world, s.view, u[VU_WORLD_VIEW]);
    if (memcmp(&prev.projection, &s.projection, sizeof(s.projection)) != 0)
        memcpy(u[VU_PROJECTION], &s.projection, 64);
    if (memcmp(prev.texture, s.texture, sizeof(s.texture)) != 0) {
        memcpy(u[VU_TEXTURE_MATRIX0], &s.texture[0], 64);
        memcpy(u[VU_TEXTURE_MATRIX1], &s.texture[1], 64);
    }
    if (memcmp(&prev.material, &s.material, sizeof(s.material)) != 0)
        Gxm_MaterialRows(s, u);
    // The lights are in eye space: a new view moves them.
    if (s.lighting && (!prev.lighting || view ||
                       memcmp(prev.lightEnabled, s.lightEnabled, sizeof(s.lightEnabled)) != 0 ||
                       memcmp(prev.lights, s.lights, sizeof(s.lights)) != 0))
        Gxm_LightRows(s, u);
    Gxm_SmallRows(s, viewport, u);
}

#endif
