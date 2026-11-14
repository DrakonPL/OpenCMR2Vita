// The plan of the sky of a frame (render/gfx_sky_plan.h): when an executor
// can draw the sky into the backdrop, and which draws after it keep the sky
// out of their pixels.

#include "render/gfx_sky_plan.h"

#include <cstdio>

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "failed line %d: %s\n", __LINE__, #x); return 1; } } while (0)

namespace {

const int kW = 960, kH = 544;

// A frame as the game records a race view: the clear, the sky, the copy of
// the backdrop, the ground, a tree, more ground.
struct Builder {
    GfxFrame frame;

    uint32_t State(bool blend, bool test, bool depthTest, bool depthWrite, int func = GFX_CMP_LESSEQUAL)
    {
        GfxDrawState s = {};
        s.viewport.x = s.viewport.y = 0;
        s.viewport.width = kW;
        s.viewport.height = kH;
        s.viewport.maxZ = 1.0f;
        s.alphaBlend = blend;
        s.alphaTest = test;
        s.depthTest = depthTest;
        s.depthWrite = depthWrite;
        s.depthFunc = func;
        frame.states.push_back(s);
        return (uint32_t)frame.states.size() - 1;
    }
    size_t Add(GfxCommand c)
    {
        frame.commands.push_back(c);
        return frame.commands.size() - 1;
    }
    size_t Clear(DWORD flags, float z = 1.0f, DWORD width = kW)
    {
        GfxCommand c = {};
        c.type = GFX_CMD_CLEAR;
        c.clearFlags = flags;
        c.clearColor = 0xff8090a0;
        c.clearZ = z;
        c.clearViewport.width = width;
        c.clearViewport.height = kH;
        c.clearViewport.maxZ = 1.0f;
        return Add(c);
    }
    size_t Draw(uint32_t state, bool transformed = false)
    {
        GfxCommand c = {};
        c.type = GFX_CMD_DRAW;
        c.state = state;
        c.transformed = transformed;
        c.vertexCount = 3;
        c.firstIndex = ~0u;
        return Add(c);
    }
    size_t Sky(bool begin)
    {
        GfxCommand c = {};
        c.type = GFX_CMD_SKY;
        c.skyBegin = begin;
        return Add(c);
    }
    size_t Backdrop()
    {
        GfxCommand c = {};
        c.type = GFX_CMD_BACKDROP;
        return Add(c);
    }
    size_t Target(GfxTexture *t)
    {
        GfxCommand c = {};
        c.type = GFX_CMD_TARGET;
        c.target = t;
        return Add(c);
    }
};

} // namespace

int main()
{
    // A race view: the sky goes into the backdrop, and the ground before
    // the tree keeps the sky out of its pixels.
    {
        Builder b;
        const uint32_t sky = b.State(true, true, false, false);
        const uint32_t ground = b.State(false, false, true, true);
        const uint32_t shadow = b.State(true, false, true, false);
        const uint32_t tree = b.State(true, true, true, true);
        b.Clear(GFX_CLEAR_TARGET | GFX_CLEAR_ZBUFFER);
        const size_t begin = b.Sky(true);
        b.Draw(sky);
        b.Draw(sky);
        const size_t end = b.Sky(false);
        const size_t backdrop = b.Backdrop();
        const size_t g0 = b.Draw(ground);
        b.Draw(shadow);
        const size_t g1 = b.Draw(ground);
        b.Draw(tree);
        b.Draw(ground);
        const GfxSkyPlan plan = Gfx_PlanSky(b.frame, kW, kH);
        REQUIRE(plan.result == GFX_SKY_PLAN_OK && plan.begin == begin && plan.end == end);
        REQUIRE(plan.clearColor == 0xff8090a0 && plan.rect.width == kW && plan.rect.height == kH);
        REQUIRE(plan.backdropAfter && plan.backdrop == backdrop);
        // The tree blends with the pixel and writes the depth: the list ends.
        REQUIRE(plan.occluders.size() == 2 && plan.occluders[0] == g0 && plan.occluders[1] == g1);
    }

    // The draws into a texture between them change nothing; a draw of 2D
    // vertices that writes no depth does not end the list.
    {
        Builder b;
        GfxTexture texture = {};
        const uint32_t sky = b.State(true, true, false, false);
        const uint32_t ground = b.State(false, false, true, true);
        const uint32_t hud = b.State(true, false, false, false);
        b.Target(&texture);
        b.Clear(GFX_CLEAR_TARGET | GFX_CLEAR_ZBUFFER);
        b.Draw(ground);
        b.Target(nullptr);
        b.Clear(GFX_CLEAR_TARGET | GFX_CLEAR_ZBUFFER);
        b.Sky(true);
        b.Draw(sky);
        b.Sky(false);
        b.Backdrop();
        b.Draw(hud, true);
        b.Target(&texture);
        b.Draw(sky);
        b.Target(nullptr);
        const size_t g = b.Draw(ground);
        const GfxSkyPlan plan = Gfx_PlanSky(b.frame, kW, kH);
        REQUIRE(plan.result == GFX_SKY_PLAN_OK && plan.backdropAfter);
        REQUIRE(plan.occluders.size() == 1 && plan.occluders[0] == g);
    }

    // The alpha test, a depth test other than "less or equal" and "always",
    // and 2D vertices that write the depth each end the list.
    {
        const struct {
            bool blend, test;
            int func;
            bool transformed;
        } enders[] = {
            { false, true, GFX_CMP_LESSEQUAL, false },
            { false, false, GFX_CMP_LESS, false },
            { false, false, GFX_CMP_LESSEQUAL, true },
        };
        for (const auto &e : enders) {
            Builder b;
            const uint32_t sky = b.State(true, true, false, false);
            const uint32_t ground = b.State(false, false, true, true);
            const uint32_t always = b.State(false, false, true, true, GFX_CMP_ALWAYS);
            const uint32_t ender = b.State(e.blend, e.test, true, true, e.func);
            b.Clear(GFX_CLEAR_TARGET | GFX_CLEAR_ZBUFFER);
            b.Sky(true);
            b.Draw(sky);
            b.Sky(false);
            b.Backdrop();
            const size_t g = b.Draw(ground);
            const size_t a = b.Draw(always);
            b.Draw(ender, e.transformed);
            b.Draw(ground);
            const GfxSkyPlan plan = Gfx_PlanSky(b.frame, kW, kH);
            REQUIRE(plan.result == GFX_SKY_PLAN_OK);
            REQUIRE(plan.occluders.size() == 2 && plan.occluders[0] == g && plan.occluders[1] == a);
        }
    }

    // An alpha test without blending (the game's ground): a draw whose test
    // passes for each pixel is listed, one whose test can fail ends the list.
    {
        Builder b;
        const uint32_t sky = b.State(true, true, false, false);
        const uint32_t tested = b.State(false, true, true, true);
        b.Clear(GFX_CLEAR_ZBUFFER);
        b.Sky(true);
        b.Draw(sky);
        b.Sky(false);
        b.Backdrop();
        const size_t solid = b.Draw(tested);
        const size_t fence = b.Draw(tested);
        b.Draw(tested);
        const GfxSkyPlan plan = Gfx_PlanSky(b.frame, kW, kH, [&](size_t i) { return i != fence; });
        REQUIRE(plan.result == GFX_SKY_PLAN_OK && plan.occluders.size() == 1 && plan.occluders[0] == solid);
        // Without the executor's answer: none.
        REQUIRE(Gfx_PlanSky(b.frame, kW, kH).occluders.empty());
    }

    // A clear of the depth after the sky ends the list; a depth cleared to
    // other than the far plane gives none.
    {
        Builder b;
        const uint32_t sky = b.State(true, true, false, false);
        const uint32_t ground = b.State(false, false, true, true);
        b.Clear(GFX_CLEAR_TARGET | GFX_CLEAR_ZBUFFER);
        b.Sky(true);
        b.Draw(sky);
        b.Sky(false);
        b.Backdrop();
        const size_t g = b.Draw(ground);
        b.Clear(GFX_CLEAR_ZBUFFER);
        b.Draw(ground);
        GfxSkyPlan plan = Gfx_PlanSky(b.frame, kW, kH);
        REQUIRE(plan.occluders.size() == 1 && plan.occluders[0] == g);
        b.frame.commands[0].clearZ = 0.5f;
        plan = Gfx_PlanSky(b.frame, kW, kH);
        REQUIRE(plan.result == GFX_SKY_PLAN_OK && plan.occluders.empty());
    }

    // A draw between the sky and the copy of the backdrop: the copy has it,
    // so the backdrop cannot be the sky alone, and a later copy would lack
    // the sky in the pixels of a stencil: no list.
    {
        Builder b;
        const uint32_t sky = b.State(true, true, false, false);
        const uint32_t ground = b.State(false, false, true, true);
        b.Clear(GFX_CLEAR_TARGET | GFX_CLEAR_ZBUFFER);
        b.Sky(true);
        b.Draw(sky);
        b.Sky(false);
        b.Draw(ground);
        b.Backdrop();
        b.Draw(ground);
        const GfxSkyPlan plan = Gfx_PlanSky(b.frame, kW, kH);
        REQUIRE(plan.result == GFX_SKY_PLAN_OK && !plan.backdropAfter && plan.occluders.empty());
    }

    // No copy of the backdrop at all: the list stands.
    {
        Builder b;
        const uint32_t sky = b.State(true, true, false, false);
        const uint32_t ground = b.State(false, false, true, true);
        b.Clear(GFX_CLEAR_TARGET | GFX_CLEAR_ZBUFFER);
        b.Sky(true);
        b.Draw(sky);
        b.Sky(false);
        b.Draw(ground);
        const GfxSkyPlan plan = Gfx_PlanSky(b.frame, kW, kH);
        REQUIRE(plan.result == GFX_SKY_PLAN_OK && !plan.backdropAfter && plan.occluders.size() == 1);
    }

    // Frames that are drawn as recorded.
    {
        Builder b;
        const uint32_t sky = b.State(true, true, false, false);
        const uint32_t layer = b.State(true, false, false, false);
        const uint32_t deep = b.State(true, false, true, true);
        b.Clear(GFX_CLEAR_TARGET | GFX_CLEAR_ZBUFFER);
        REQUIRE(Gfx_PlanSky(b.frame, kW, kH).result == GFX_SKY_NONE);
        // A draw over the clear before the sky (the background layer).
        b.Draw(layer, true);
        b.Sky(true);
        b.Draw(sky);
        b.Sky(false);
        REQUIRE(Gfx_PlanSky(b.frame, kW, kH).result == GFX_SKY_DRAWN_UNDER);
        // A second clear after that draw: the sky is on the clear again.
        b.frame.commands.insert(b.frame.commands.begin() + 2, b.frame.commands[0]);
        REQUIRE(Gfx_PlanSky(b.frame, kW, kH).result == GFX_SKY_PLAN_OK);
        // A second sky (two views).
        b.Sky(true);
        b.Draw(sky);
        b.Sky(false);
        REQUIRE(Gfx_PlanSky(b.frame, kW, kH).result == GFX_SKY_MANY);
    }
    {
        // A race: the game clears only the depth, and the dome of the sky
        // covers an older frame. The backdrop starts black; the ground after
        // the sky is listed.
        Builder b;
        const uint32_t sky = b.State(true, true, false, false);
        const uint32_t ground = b.State(false, false, true, true);
        b.Clear(GFX_CLEAR_ZBUFFER);
        b.Sky(true);
        b.Draw(sky);
        b.Sky(false);
        b.Backdrop();
        const size_t g = b.Draw(ground);
        GfxSkyPlan plan = Gfx_PlanSky(b.frame, kW, kH);
        REQUIRE(plan.result == GFX_SKY_PLAN_OK && plan.clearColor == 0xff000000 && plan.backdropAfter);
        REQUIRE(plan.occluders.size() == 1 && plan.occluders[0] == g);
        // A draw before the sky with no clear of the colour at all.
        b.frame.commands.insert(b.frame.commands.begin(), b.frame.commands[g]);
        REQUIRE(Gfx_PlanSky(b.frame, kW, kH).result == GFX_SKY_DRAWN_UNDER);
        b.frame.commands.erase(b.frame.commands.begin());
        b.frame.commands.resize(4);
        // A clear of the colour over a part of the screen.
        b.frame.commands[0].clearFlags = GFX_CLEAR_TARGET;
        b.frame.commands[0].clearViewport.width = kW / 2;
        REQUIRE(Gfx_PlanSky(b.frame, kW, kH).result == GFX_SKY_NO_CLEAR);
        b.frame.commands[0].clearViewport.width = kW;
        REQUIRE(Gfx_PlanSky(b.frame, kW, kH).result == GFX_SKY_PLAN_OK);
        // A sky draw that writes the depth.
        b.frame.states[sky].depthTest = b.frame.states[sky].depthWrite = true;
        REQUIRE(Gfx_PlanSky(b.frame, kW, kH).result == GFX_SKY_OTHER_DRAW);
        b.frame.states[sky].depthTest = b.frame.states[sky].depthWrite = false;
        // A sky without its end, and a sky with no draw.
        b.frame.commands.pop_back();
        REQUIRE(Gfx_PlanSky(b.frame, kW, kH).result == GFX_SKY_NONE);
        b.frame.commands.pop_back();
        b.Sky(false);
        REQUIRE(Gfx_PlanSky(b.frame, kW, kH).result == GFX_SKY_NONE);
    }
    // A sky in a texture is not the back buffer's.
    {
        Builder b;
        GfxTexture texture = {};
        const uint32_t sky = b.State(true, true, false, false);
        b.Target(&texture);
        b.Clear(GFX_CLEAR_TARGET | GFX_CLEAR_ZBUFFER);
        b.Sky(true);
        b.Draw(sky);
        b.Sky(false);
        b.Target(nullptr);
        REQUIRE(Gfx_PlanSky(b.frame, kW, kH).result == GFX_SKY_NONE);
    }
    printf("gfx_sky_plan: ok\n");
    return 0;
}
