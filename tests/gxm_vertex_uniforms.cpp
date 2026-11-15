// The vertex uniforms of the GXM executor (src/render/gxm_vertex_uniforms.h):
// an update from the state before gives the rows of a full fill, through a
// chain of states that each change one part. The update tells whether a row
// changed: a state that differs only in what the vertex program does not read
// (a texture, the blending) changes none. No GXM.

#include "render/gxm_vertex_uniforms.h"

#include <cstdio>
#include <cstring>

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "failed line %d: %s\n", __LINE__, #x); return 1; } } while (0)

namespace {

GfxMatrix Matrix(float seed)
{
    GfxMatrix m{};
    float *p = &m._11;
    for (int i = 0; i < 16; i++)
        p[i] = seed + 0.25f * (float)i;
    return m;
}

float s_updated[VU_ROWS][4];    // the rows the chain of updates gives
GfxDrawState s_before;          // the state they are of
bool s_changed;                 // what the last update returned

// Updates the rows to state and checks them against a full fill: the rows
// before the lights always, the light rows with lighting. An update that
// reports no change leaves those rows as they were.
bool Step(const GfxDrawState &state, const GfxViewport &viewport)
{
    float filled[VU_ROWS][4], before[VU_ROWS][4];
    memcpy(before, s_updated, sizeof(before));
    Gxm_FillVertexUniforms(state, viewport, filled);
    s_changed = Gxm_UpdateVertexUniforms(s_before, state, viewport, s_updated);
    s_before = state;
    const int rows = state.lighting ? VU_ROWS : VU_UNLIT_ROWS;
    if (!s_changed && memcmp(before, s_updated, sizeof(float) * 4 * rows) != 0)
        return false;
    return memcmp(filled, s_updated, sizeof(float) * 4 * rows) == 0;
}

} // namespace

int main()
{
    // The lights are the last rows: a draw without lighting uploads the rows
    // before them.
    REQUIRE(VU_UNLIT_ROWS == VU_LIGHTS && VU_LIGHTS + 8 * VU_LIGHT_ROWS == VU_ROWS);
    for (int row : { VU_WORLD_VIEW, VU_PROJECTION, VU_TEXTURE_MATRIX1, VU_MATERIAL_EMISSIVE, VU_GLOBAL_AMBIENT,
                     VU_LIGHT_FLAGS, VU_SOURCES, VU_FOG_FLAGS, VU_FOG_PARAMS, VU_TEX_COORD, VU_TARGET,
                     VU_TEX_GEN1 + 1 })
        REQUIRE(row < VU_UNLIT_ROWS);

    GfxViewport viewport{};
    viewport.width = 960;
    viewport.height = 544;
    GfxDrawState state;
    state.lighting = false;
    state.world = Matrix(1.0f);
    state.view = Matrix(2.0f);
    state.projection = Matrix(3.0f);
    Gxm_FillVertexUniforms(state, viewport, s_updated);
    s_before = state;

    // Each part alone, without lighting.
    state.world = Matrix(4.0f);
    REQUIRE(Step(state, viewport) && s_changed);
    state.view = Matrix(5.0f);
    REQUIRE(Step(state, viewport) && s_changed);
    state.projection = Matrix(6.0f);
    REQUIRE(Step(state, viewport) && s_changed);
    state.texture[1] = Matrix(7.0f);
    REQUIRE(Step(state, viewport) && s_changed);
    state.material.diffuse.a = 0.5f;
    state.material.power = 12.0f;
    REQUIRE(Step(state, viewport) && s_changed);
    // The same state again, another texture, another blending: the vertex
    // program reads none of them, and no row changes.
    REQUIRE(Step(state, viewport) && !s_changed);
    GfxTexture road{}, grass{};
    state.stages[0].texture = &road;
    REQUIRE(Step(state, viewport) && !s_changed);
    state.stages[0].texture = &grass;
    state.stages[1].texture = &road;
    state.alphaBlend = true;
    state.depthWrite = false;
    state.alphaRef = 0x80;
    REQUIRE(Step(state, viewport) && !s_changed);
    // A light that changes without lighting changes no row that is read.
    state.lights[1].range = 60.0f;
    state.lightEnabled[1] = true;
    REQUIRE(Step(state, viewport) && !s_changed);
    state.lightEnabled[1] = false;
    REQUIRE(Step(state, viewport) && !s_changed);
    state.ambient = 0x80402010;
    state.fog = true;
    state.fogVertexMode = GFX_FOG_LINEAR;
    state.fogStart = 10.0f;
    state.fogEnd = 90.0f;
    state.stages[1].texCoordIndex = 0x30001;
    state.stages[0].textureTransform = GFX_TTFF_DISABLE + 2;
    REQUIRE(Step(state, viewport) && s_changed);
    // Each of the small rows alone.
    state.fogEnd = 120.0f;
    REQUIRE(Step(state, viewport) && s_changed);
    state.normalizeNormals = true;
    REQUIRE(Step(state, viewport) && s_changed);
    state.diffuseSource = GFX_MCS_MATERIAL;
    REQUIRE(Step(state, viewport) && s_changed);
    // The rows of the second texture coordinates, then their flag alone.
    state.texGen1 = true;
    state.texGen1Rows[1][2] = -0.25f;
    REQUIRE(Step(state, viewport) && s_changed);
    state.texGen1Rows[0][3] = 0.5f;
    REQUIRE(Step(state, viewport) && s_changed);
    state.texGen1 = false;
    REQUIRE(Step(state, viewport) && s_changed);
    viewport.width = 256;
    REQUIRE(Step(state, viewport) && s_changed);

    // Lighting on: the light rows become current, with the lights that are on.
    state.lighting = true;
    state.lightEnabled[2] = true;
    state.lights[2].type = 3;
    state.lights[2].diffuse.r = 0.75f;
    state.lights[2].direction.z = 1.0f;
    state.lights[2].theta = 0.6f;
    REQUIRE(Step(state, viewport) && s_changed);
    // With lighting also, another texture changes no row.
    state.stages[0].texture = &road;
    REQUIRE(Step(state, viewport) && !s_changed);
    // A light changes, another goes on, one goes off.
    state.lights[2].diffuse.g = 0.25f;
    REQUIRE(Step(state, viewport) && s_changed);
    state.lightEnabled[5] = true;
    state.lights[5].type = 1;
    state.lights[5].position.x = 30.0f;
    state.lights[5].range = 50.0f;
    REQUIRE(Step(state, viewport));
    state.lightEnabled[2] = false;
    REQUIRE(Step(state, viewport));
    // The lights are in eye space: a new view moves them.
    state.view = Matrix(8.0f);
    REQUIRE(Step(state, viewport));
    // Only the world changes: the light rows stay as they are, and are right.
    state.world = Matrix(9.0f);
    REQUIRE(Step(state, viewport));

    // Lighting off, the lights and the view change, lighting on again: the
    // rows are those of the new lights and view.
    state.lighting = false;
    REQUIRE(Step(state, viewport));
    state.view = Matrix(10.0f);
    state.lights[5].position.x = -30.0f;
    state.lightEnabled[0] = true;
    state.lights[0].type = 3;
    REQUIRE(Step(state, viewport));
    state.lighting = true;
    REQUIRE(Step(state, viewport));

    printf("gxm_vertex_uniforms: ok\n");
    return 0;
}
