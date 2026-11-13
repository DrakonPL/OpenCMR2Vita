// The small batches of a frame made draws (gfx_internal.h,
// Gfx_ExpandSmallBatches). The recorder keeps the small draws of a batch as
// the game made them (Gfx_DrawBatch), with their vertices in the game's
// vertex buffers. Here each one gets what the recorder gives a small draw
// made alone: its vertices in world space, the view test, its distance, the
// coordinates of the atlas, and a join with the draw before it. With the
// threaded executor this runs on the worker thread.

#include "render/gfx_internal.h"
#include "render/gfx_small_draw.h"

#include <string.h>

namespace {

const GfxMatrix kIdentity = { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                              0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f };

// Vertices at a multiple of their size at the end of the frame's data (as
// the recorder appends them), so that a draw starts at its first vertex.
uint32_t AppendVertices(GfxFrame &frame, const GfxVertex *vertices, DWORD count)
{
    const size_t stride = sizeof(GfxVertex);
    const size_t offset = (frame.vertices.size() + stride - 1) / stride * stride;
    frame.vertices.resize(offset);
    frame.vertices.insert(frame.vertices.end(), (const uint8_t *)vertices, (const uint8_t *)(vertices + count));
    return (uint32_t)offset;
}

} // namespace

void Gfx_ExpandSmallBatches(GfxFrame &frame)
{
    bool batches = false;
    for (const GfxCommand &c : frame.commands)
        batches = batches || c.type == GFX_CMD_SMALL_BATCH;
    if (!batches)
        return;
    std::vector<GfxCommand> commands;
    commands.reserve(frame.commands.size() + frame.smallItems.size() / 4 + 1);
    for (const GfxCommand &c : frame.commands) {
        if (c.type != GFX_CMD_SMALL_BATCH) {
            commands.push_back(c);
            continue;
        }
        const GfxDrawState &s = frame.states[c.state];
        float clip[16];
        if (c.cull)
            Gfx_ClipMatrixOf(kIdentity, s.view, s.projection, clip);
        // The draw of this batch that the next item joins, if any.
        size_t run = commands.size();
        for (uint32_t k = c.firstItem; k < c.firstItem + c.itemCount && k < frame.smallItems.size(); k++) {
            const GfxSmallItem &item = frame.smallItems[k];
            const DWORD count = item.vertexCount;
            if (count == 0 || count > kGfxSmallDrawVertices || item.buffer == NULL ||
                item.firstVertex + count > item.buffer->vertices.size() ||
                item.firstIndex + item.indexCount > frame.smallIndices.size())
                continue;
            // The vertices of the game's buffer: the recorder runs this
            // before the game writes them again.
            const GfxVertex *source = &item.buffer->vertices[item.firstVertex];
            GfxVertex v[kGfxSmallDrawVertices];
            Gfx_SmallDrawToWorld(item.world, source, count, v);
            if (c.cull && Gfx_PointsOutsideView(clip, v, count))
                continue;
            const float reach = Gfx_SmallDrawReach(s.view, v, count);
            // The atlas, for coordinates inside the texture.
            GfxTexture *texture = item.texture;
            if (item.atlas != NULL && Gfx_AtlasCoordinatesFit(v, count)) {
                Gfx_AtlasRemap(v, count, item.atlasX, item.atlasY, item.textureWidth, item.textureHeight,
                               item.atlasScale);
                texture = item.atlas;
            }
            float low[3], high[3];
            Gfx_VertexBox(v, count, low, high);
            const uint16_t *indices = &frame.smallIndices[item.firstIndex];
            if (run < commands.size()) {
                GfxCommand &last = commands[run];
                if (last.textures[0] == texture && last.vertexCount + count <= kGfxJoinedVertices &&
                    last.indexCount % 3 == 0) {
                    AppendVertices(frame, v, count);
                    for (uint32_t i = 0; i < item.indexCount; i++)
                        frame.indices.push_back((uint16_t)(indices[i] + last.vertexCount));
                    last.indexCount += item.indexCount;
                    last.vertexCount += count;
                    for (int a = 0; a < 3; a++) {
                        last.boxLow[a] = low[a] < last.boxLow[a] ? low[a] : last.boxLow[a];
                        last.boxHigh[a] = high[a] > last.boxHigh[a] ? high[a] : last.boxHigh[a];
                    }
                    last.reach = reach > last.reach ? reach : last.reach;
                    continue;
                }
            }
            GfxCommand draw = {};
            draw.type = GFX_CMD_DRAW;
            draw.primitive = GFX_TRIANGLELIST;
            draw.transformed = false;
            draw.buffer = NULL;
            draw.firstVertexByte = AppendVertices(frame, v, count);
            draw.vertexCount = count;
            draw.firstIndex = (uint32_t)frame.indices.size();
            draw.indexCount = item.indexCount;
            frame.indices.insert(frame.indices.end(), indices, indices + item.indexCount);
            draw.state = c.state;
            draw.textures[0] = texture;
            draw.textures[1] = c.textures[1];
            draw.hasBox = true;
            memcpy(draw.boxLow, low, sizeof(draw.boxLow));
            memcpy(draw.boxHigh, high, sizeof(draw.boxHigh));
            draw.reach = reach;
            commands.push_back(draw);
            run = commands.size() - 1;
        }
    }
    frame.commands.swap(commands);
    frame.smallItems.clear();
    frame.smallIndices.clear();
}
