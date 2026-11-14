// The small batches of a frame made draws (gfx_internal.h). The recorder
// keeps the small draws of a batch as the game made them (Gfx_DrawBatch),
// with their vertices in the game's vertex buffers. A batch job
// (Gfx_RunBatchJob) gives each one what the recorder gives a small draw made
// alone: its vertices in world space, the view test, its distance, the
// coordinates of the atlas, and a join with the draw before it. The jobs run
// on a thread of their own while the game records the rest of the frame
// (gfx_batch_worker.cpp). Gfx_SpliceSmallBatches then puts the draws in the
// place of their batch among the frame's commands.

#include "render/gfx_internal.h"
#include "render/gfx_small_draw.h"

#include <string.h>

namespace {

const GfxMatrix kIdentity = { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                              0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f };

constexpr size_t kNoRun = ~(size_t)0;

} // namespace

void Gfx_RunBatchJob(const GfxBatchJob &job)
{
    GfxBatchData &data = *job.data;
    // The draw of this batch that the next draw joins, if any: that of the
    // job before, for a job of the same batch.
    if (data.runPlaceholder != job.placeholder) {
        data.runPlaceholder = job.placeholder;
        data.run = kNoRun;
    }
    float clip[16];
    if (job.cull)
        Gfx_ClipMatrixOf(kIdentity, job.view, job.projection, clip);
    for (uint32_t k = 0; k < job.itemCount; k++) {
        const GfxSmallItem &item = job.items[k];
        const DWORD count = item.vertexCount;
        if (count == 0 || count > kGfxSmallDrawVertices || item.buffer == NULL ||
            item.firstVertex + count > item.buffer->vertices.size() ||
            item.firstIndex + item.indexCount > job.indexCount)
            continue;
        // The vertices of the game's buffer: the recorder waits for this
        // job before the game writes them again.
        const GfxVertex *source = &item.buffer->vertices[item.firstVertex];
        GfxVertex v[kGfxSmallDrawVertices];
        Gfx_SmallDrawToWorld(item.world, source, count, v);
        if (job.cull && Gfx_PointsOutsideView(clip, v, count))
            continue;
        const float reach = Gfx_SmallDrawReach(job.view, v, count);
        // The atlas, for coordinates inside the texture.
        GfxTexture *texture = item.texture;
        if (item.atlas != NULL && Gfx_AtlasCoordinatesFit(v, count)) {
            Gfx_AtlasRemap(v, count, item.atlasX, item.atlasY, item.textureWidth, item.textureHeight,
                           item.atlasScale);
            texture = item.atlas;
        }
        float low[3], high[3];
        Gfx_VertexBox(v, count, low, high);
        const uint16_t *indices = job.indices + item.firstIndex;
        if (data.run != kNoRun) {
            GfxCommand &last = data.draws[data.run].draw;
            if (last.textures[0] == texture && last.vertexCount + count <= kGfxJoinedVertices &&
                last.indexCount % 3 == 0) {
                data.vertices.insert(data.vertices.end(), v, v + count);
                for (uint32_t i = 0; i < item.indexCount; i++)
                    data.drawIndices.push_back((uint16_t)(indices[i] + last.vertexCount));
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
        GfxBatchOutput made = {};
        made.placeholder = job.placeholder;
        GfxCommand &draw = made.draw;
        draw.part = job.part;
        draw.type = GFX_CMD_DRAW;
        draw.primitive = GFX_TRIANGLELIST;
        draw.transformed = false;
        draw.buffer = NULL;
        // Places in the vertices and the indices of the batches: those of
        // the frame's data come with the splice.
        draw.firstVertexByte = (uint32_t)(data.vertices.size() * sizeof(GfxVertex));
        draw.vertexCount = count;
        draw.firstIndex = (uint32_t)data.drawIndices.size();
        draw.indexCount = item.indexCount;
        data.vertices.insert(data.vertices.end(), v, v + count);
        data.drawIndices.insert(data.drawIndices.end(), indices, indices + item.indexCount);
        draw.state = job.state;
        draw.textures[0] = texture;
        draw.textures[1] = job.texture1;
        draw.hasBox = true;
        memcpy(draw.boxLow, low, sizeof(draw.boxLow));
        memcpy(draw.boxHigh, high, sizeof(draw.boxHigh));
        draw.reach = reach;
        data.draws.push_back(made);
        data.run = data.draws.size() - 1;
    }
}

void Gfx_SpliceSmallBatches(GfxFrame &frame)
{
    bool batches = false;
    for (const GfxCommand &c : frame.commands)
        batches = batches || c.type == GFX_CMD_SMALL_BATCH;
    if (!batches)
        return;
    const GfxBatchOutput *draws = frame.batch != nullptr ? frame.batch->draws.data() : nullptr;
    const size_t drawCount = frame.batch != nullptr ? frame.batch->draws.size() : 0;
    // The vertices and the indices of the batches after those of the frame,
    // the vertices at a multiple of their size (a draw starts at its first
    // vertex).
    uint32_t vertexBase = 0, indexBase = 0;
    if (drawCount > 0) {
        const GfxBatchData &data = *frame.batch;
        const size_t stride = sizeof(GfxVertex);
        vertexBase = (uint32_t)((frame.vertices.size() + stride - 1) / stride * stride);
        frame.vertices.resize(vertexBase);
        frame.vertices.insert(frame.vertices.end(), (const uint8_t *)data.vertices.data(),
                              (const uint8_t *)(data.vertices.data() + data.vertices.size()));
        indexBase = (uint32_t)frame.indices.size();
        frame.indices.insert(frame.indices.end(), data.drawIndices.begin(), data.drawIndices.end());
    }
    // The draws of each batch in its place: they are in the order of the
    // batches.
    std::vector<GfxCommand> commands;
    commands.reserve(frame.commands.size() + drawCount);
    size_t next = 0;
    for (size_t i = 0; i < frame.commands.size(); i++) {
        const GfxCommand &c = frame.commands[i];
        if (c.type != GFX_CMD_SMALL_BATCH) {
            commands.push_back(c);
            continue;
        }
        for (; next < drawCount && draws[next].placeholder <= i; next++) {
            if (draws[next].placeholder != i)
                continue;
            GfxCommand draw = draws[next].draw;
            draw.firstVertexByte += vertexBase;
            draw.firstIndex += indexBase;
            commands.push_back(draw);
        }
    }
    frame.commands.swap(commands);
    if (frame.batch != nullptr)
        frame.batch->Reset();
}
