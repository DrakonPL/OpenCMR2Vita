// A rectangle of the render target as the viewport GXM takes: a centre and a
// scale per axis. Clip y is up and the target's row 0 is at the top, as in
// Direct3D; clip z 0..1 goes to minZ..maxZ. No GXM here:
// tests/gxm_viewport.cpp checks it.
#ifndef OPENCMR2_GXM_VIEWPORT_H
#define OPENCMR2_GXM_VIEWPORT_H

struct GxmViewport {
    float xOffset, xScale, yOffset, yScale, zOffset, zScale;
};

inline GxmViewport Gxm_ViewportFromRect(float x, float y, float width, float height, float minZ, float maxZ)
{
    return { x + width * 0.5f, width * 0.5f, y + height * 0.5f, -height * 0.5f, minZ, maxZ - minZ };
}

#endif
