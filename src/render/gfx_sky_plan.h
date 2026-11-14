// How an executor can draw the sky of a frame in another way (Gfx_MarkSky).
// In a wet stage the six blended layers of the sky cost 7 to 9.5 ms of GPU
// time in each frame on the PS Vita: each layer is drawn over most of the
// screen, also where the ground covers it later.
//
// Two changes keep the picture of the frame:
// - The backdrop of the distance fade is a copy of the back buffer after the
//   sky. The executor can draw the sky into the backdrop itself, at its
//   size: no copy and no split of the back buffer's scene. That needs no
//   draw in the back buffer before the sky (after its last clear of the
//   colour), and no draw between the sky and the copy. In a race the game
//   clears only the depth: the back buffer holds an older frame, which the
//   dome of the sky covers. The backdrop then starts black.
// - The draws that come first after the sky and write the depth without
//   blending and without an alpha test that can fail (the ground: the game
//   leaves the test on, and the executor knows the least alpha of the
//   textures and the vertices) cover their pixels for good: no later draw
//   shows the sky through them. Their stencil, drawn
//   before the sky, keeps the sky out of those pixels. A draw that blends
//   with the pixel and writes the depth (a tree) ends the list: before it a
//   pixel can still show the sky. A draw that fades into the backdrop reads
//   the backdrop, not the back buffer: it needs the first change.
//
// No GXM here: tests/gfx_sky_plan.cpp checks it on the PC.
#ifndef OPENCMR2_GFX_SKY_PLAN_H
#define OPENCMR2_GFX_SKY_PLAN_H

#include "render/gfx_executor_shared.h"
#include "render/gfx_internal.h"

#include <stddef.h>
#include <stdint.h>

#include <vector>

enum GfxSkyPlanResult {
    GFX_SKY_PLAN_OK,
    GFX_SKY_NONE,           // no sky in the back buffer
    GFX_SKY_MANY,           // more than one (two views): drawn as recorded
    GFX_SKY_NO_CLEAR,       // a clear of the colour of only a part of the sky's rectangle
    GFX_SKY_DRAWN_UNDER,    // a draw between the clear and the sky
    GFX_SKY_OTHER_DRAW,     // a draw of the sky that writes the depth, or of 2D vertices
    GFX_SKY_RESULT_COUNT
};

struct GfxSkyPlan {
    int result = GFX_SKY_NONE;
    size_t begin = 0, end = 0;      // the commands of the markers
    // The colour under the sky: that of the last clear of the colour, or
    // black without one.
    DWORD clearColor = 0;
    GfxViewport rect = {};          // the sky's rectangle, in back buffer pixels
    // The back buffer copy of the backdrop comes right after the sky: the
    // backdrop can be the sky alone.
    bool backdropAfter = false;
    size_t backdrop = 0;            // that command
    // The draws whose stencil keeps the sky out of their pixels, in order.
    // With backdropAfter, only for an executor that draws the sky into the
    // backdrop: a copy of a back buffer without the sky in their pixels
    // would fade the scenery into the colour of the clear there.
    std::vector<uint32_t> occluders;
};

namespace GfxSkyPlanDetail {

inline bool Contains(const GfxViewport &outer, const GfxViewport &inner)
{
    return inner.x >= outer.x && inner.y >= outer.y && inner.x + inner.width <= outer.x + outer.width &&
           inner.y + inner.height <= outer.y + outer.height;
}

} // namespace GfxSkyPlanDetail

// The plan of a frame whose back buffer is backW x backH pixels.
// alphaTestPasses(i): whether the alpha test of the draw commands[i] passes
// for each of its pixels.
template <typename AlphaTestPasses>
GfxSkyPlan Gfx_PlanSky(const GfxFrame &frame, int backW, int backH, AlphaTestPasses alphaTestPasses)
{
    using GfxSkyPlanDetail::Contains;
    GfxSkyPlan plan;
    const std::vector<GfxCommand> &commands = frame.commands;
    // The markers of the back buffer.
    bool backBuffer = true;
    int begins = 0;
    bool open = false;
    for (size_t i = 0; i < commands.size(); i++) {
        const GfxCommand &c = commands[i];
        if (c.type == GFX_CMD_TARGET)
            backBuffer = c.target == nullptr;
        if (!backBuffer || c.type != GFX_CMD_SKY)
            continue;
        if (c.skyBegin) {
            begins++;
            if (begins == 1) {
                plan.begin = i;
                open = true;
            }
        } else if (open) {
            plan.end = i;
            open = false;
        }
    }
    if (begins == 0 || open) {
        plan.result = GFX_SKY_NONE;
        return plan;
    }
    if (begins > 1) {
        plan.result = GFX_SKY_MANY;
        return plan;
    }
    // The draws of the sky: 3D vertices, no depth written, and their
    // rectangle (that of the first).
    bool any = false;
    for (size_t i = plan.begin + 1; i < plan.end; i++) {
        const GfxCommand &c = commands[i];
        if (c.type != GFX_CMD_DRAW)
            continue;
        const GfxDrawState &s = frame.states[c.state];
        if (c.transformed || (s.depthTest && s.depthWrite)) {
            plan.result = GFX_SKY_OTHER_DRAW;
            return plan;
        }
        if (!any)
            plan.rect = GfxShared::ToTarget(s.viewport, s.screenW, s.screenH, true, backW, backH);
        any = true;
    }
    if (!any) {
        plan.result = GFX_SKY_NONE;
        return plan;
    }
    // Under the sky: the colour of the last clear of the colour, or black
    // without one, and no draw after it (or since the start of the frame).
    // Its depth: cleared to the far plane, for the stencil.
    backBuffer = true;
    bool colourCleared = false, colourCovers = false, depthCleared = false, drawnUnder = false;
    for (size_t i = 0; i < plan.begin; i++) {
        const GfxCommand &c = commands[i];
        if (c.type == GFX_CMD_TARGET) {
            backBuffer = c.target == nullptr;
            continue;
        }
        if (!backBuffer)
            continue;
        if (c.type == GFX_CMD_CLEAR) {
            const GfxViewport rect =
                GfxShared::ToTarget(c.clearViewport, c.clearScreenW, c.clearScreenH, true, backW, backH);
            const bool covers = Contains(rect, plan.rect);
            if ((c.clearFlags & GFX_CLEAR_TARGET) != 0) {
                colourCleared = true;
                colourCovers = covers;
                plan.clearColor = c.clearColor;
                drawnUnder = false;
            }
            if ((c.clearFlags & GFX_CLEAR_ZBUFFER) != 0)
                depthCleared = covers && c.clearZ == 1.0f;
        } else if (c.type == GFX_CMD_DRAW) {
            drawnUnder = true;
        }
    }
    if (colourCleared && !colourCovers) {
        plan.result = GFX_SKY_NO_CLEAR;
        return plan;
    }
    if (!colourCleared)
        plan.clearColor = 0xff000000;
    if (drawnUnder) {
        plan.result = GFX_SKY_DRAWN_UNDER;
        return plan;
    }
    // After the sky: the copy of the backdrop, and the draws that cover
    // their pixels for good. A depth write that is not one of them, a clear
    // of the depth, a new target of the back buffer, or a later sky ends the
    // list.
    bool listing = depthCleared;
    bool drawnAfter = false, anyBackdrop = false;
    backBuffer = true;
    for (size_t i = plan.end + 1; i < commands.size(); i++) {
        const GfxCommand &c = commands[i];
        if (c.type == GFX_CMD_TARGET) {
            backBuffer = c.target == nullptr;
            continue;
        }
        if (!backBuffer)
            continue;
        if (c.type == GFX_CMD_BACKDROP) {
            anyBackdrop = true;
            if (!drawnAfter && !plan.backdropAfter) {
                plan.backdropAfter = true;
                plan.backdrop = i;
            }
            continue;
        }
        if (c.type == GFX_CMD_CLEAR) {
            if ((c.clearFlags & GFX_CLEAR_ZBUFFER) != 0)
                listing = false;
            continue;
        }
        if (c.type == GFX_CMD_SKY) {
            listing = false;
            continue;
        }
        if (c.type != GFX_CMD_DRAW)
            continue;
        drawnAfter = true;
        const GfxDrawState &s = frame.states[c.state];
        if (!listing || !(s.depthTest && s.depthWrite))
            continue;
        const bool covers = !c.transformed && !s.alphaBlend &&
                            (s.depthFunc == GFX_CMP_LESSEQUAL || s.depthFunc == GFX_CMP_ALWAYS) &&
                            (!s.alphaTest || alphaTestPasses(i));
        if (!covers) {
            listing = false;
            continue;
        }
        plan.occluders.push_back((uint32_t)i);
    }
    // A copy of the backdrop later in the frame: it takes the back buffer,
    // which would lack the sky in the pixels of the stencil.
    if (anyBackdrop && !plan.backdropAfter)
        plan.occluders.clear();
    plan.result = GFX_SKY_PLAN_OK;
    return plan;
}

// The plan with no alpha test known to pass.
inline GfxSkyPlan Gfx_PlanSky(const GfxFrame &frame, int backW, int backH)
{
    return Gfx_PlanSky(frame, backW, backH, [](size_t) { return false; });
}

#endif
