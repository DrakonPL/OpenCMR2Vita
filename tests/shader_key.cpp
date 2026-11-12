// The draw keys of src/render/shader_key.h: known states give known keys,
// and what stays a uniform (arguments, colours, distances) leaves them alone.

#include "render/shader_key.h"

#include <cstdio>

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "failed line %d: %s\n", __LINE__, #x); return 1; } } while (0)

int main()
{
    GfxTexture sprite{}, scenery{}, cube{};
    sprite.format = scenery.format = cube.format = GFX_FORMAT_BGRA8;
    cube.flags = GFX_TEXTURE_CUBE;

    // A HUD sprite: texture times diffuse, alpha tested, point filtered.
    GfxDrawState hud;
    hud.stages[0].colorOp = GFX_TOP_MODULATE;
    hud.stages[0].alphaOp = GFX_TOP_MODULATE;
    hud.stages[0].texture = &sprite;
    hud.stages[1].colorOp = GFX_TOP_DISABLE;
    hud.alphaTest = true;
    REQUIRE(Gfx_FragmentKey(hud, false) == 0x10006484ull);

    // Scenery with a reflection: 2D texture with mipmaps, then a cube map;
    // table fog, specular and the distance fade.
    GfxDrawState stage;
    stage.stages[0].colorOp = GFX_TOP_MODULATE;
    stage.stages[0].alphaOp = GFX_TOP_SELECTARG1;
    stage.stages[0].texture = &scenery;
    stage.stages[0].mipFilter = GFX_MIPFILTER_LINEAR;
    stage.stages[1].colorOp = GFX_TOP_MODULATE;
    stage.stages[1].alphaOp = GFX_TOP_DISABLE;
    stage.stages[1].texture = &cube;
    stage.fog = true;
    stage.fogTableMode = GFX_FOG_LINEAR;
    stage.specular = true;
    stage.fadeStart = 100.0f;
    stage.fadeEnd = 200.0f;
    REQUIRE(Gfx_FragmentKey(stage, true) == 0x1e2090444ull);
    // The fade applies to 3D draws into the back buffer only.
    REQUIRE(Gfx_FragmentKey(stage, false) == 0x0e2090444ull);

    // Arguments and values are uniforms: the same program draws them.
    GfxDrawState other = stage;
    other.stages[0].colorArg1 = GFX_TA_DIFFUSE;
    other.stages[0].colorArg2 = GFX_TA_TFACTOR | GFX_TA_COMPLEMENT;
    other.stages[1].alphaArg1 = GFX_TA_SPECULAR;
    other.textureFactor = 0x80402010;
    other.alphaFunc = GFX_CMP_LESSEQUAL;
    other.alphaRef = 0x7f;
    other.fogColor = 0x00ff00ff;
    other.fogStart = 5.0f;
    other.fogEnd = 50.0f;
    other.fadeStart = 10.0f;
    other.fadeEnd = 20.0f;
    other.stages[0].bumpMat[0] = 0.5f;
    REQUIRE(Gfx_FragmentKey(other, true) == Gfx_FragmentKey(stage, true));

    // Stage 1 does not run when stage 0 is off.
    GfxDrawState off = stage;
    off.stages[0].colorOp = GFX_TOP_DISABLE;
    GfxDrawState offOther = off;
    offOther.stages[1].colorOp = GFX_TOP_ADD;
    offOther.stages[1].texture = nullptr;
    REQUIRE(Gfx_FragmentKey(off, true) == Gfx_FragmentKey(offOther, true));

    // An operation or a texture kind is a different program.
    GfxDrawState add = stage;
    add.stages[0].colorOp = GFX_TOP_ADD;
    REQUIRE(Gfx_FragmentKey(add, true) != Gfx_FragmentKey(stage, true));
    GfxDrawState noCube = stage;
    noCube.stages[1].texture = &scenery;
    REQUIRE(Gfx_FragmentKey(noCube, true) != Gfx_FragmentKey(stage, true));

    // A blended, untested, pre-transformed list: one of the game's pipelines.
    GfxDrawState blended;
    blended.alphaBlend = true;
    blended.srcBlend = GFX_BLEND_SRCALPHA;
    blended.destBlend = GFX_BLEND_INVSRCALPHA;
    blended.depthTest = false;
    REQUIRE(Gfx_PipelineKey(GFX_TRIANGLELIST, true, blended) == 0x1a0cb9u);
    puts("shader_key: ok");
    return 0;
}
