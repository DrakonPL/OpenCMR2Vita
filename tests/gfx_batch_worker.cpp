// The batch jobs of the recorder (src/render/gfx_batch_worker.cpp): without
// the thread a job runs when it is added. With the thread the jobs run one
// at a time and in their order: the draws they make are those of the same
// jobs run in place, also for more jobs than the queue holds. A wait for a
// job returns when the job is done: vertices that the caller then writes
// are not in its draws.

#include "render/gfx_batch_worker.h"
#include "render/gfx_internal.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "failed line %d: %s\n", __LINE__, #x); return 1; } } while (0)

extern "C" void Sys_Log(const char *, ...) {}

namespace {

// A batch data with count triangles of the buffer, each moved by 2 on x,
// and jobs of jobItems triangles each: batches of batchJobs jobs.
struct Batches {
    std::shared_ptr<GfxBatchData> data = std::make_shared<GfxBatchData>();
    std::vector<GfxBatchJob> jobs;

    Batches(const GfxVertexBuffer &buffer, uint32_t count, uint32_t jobItems, uint32_t batchJobs)
    {
        GfxSmallItem item = {};
        item.world._11 = item.world._22 = item.world._33 = item.world._44 = 1.0f;
        item.world._41 = 2.0f;
        item.buffer = &buffer;
        item.vertexCount = 3;
        item.indexCount = 3;
        for (uint32_t i = 0; i < count; i++) {
            item.firstVertex = (i % 4) * 3;
            item.firstIndex = (uint32_t)data->indices.size();
            data->items.push_back(item);
            data->indices.insert(data->indices.end(), { 0, 1, 2 });
        }
        for (uint32_t first = 0; first < count; first += jobItems) {
            GfxBatchJob job = {};
            job.data = data.get();
            job.items = data->items.data() + first;
            job.itemCount = first + jobItems <= count ? jobItems : count - first;
            job.indices = data->indices.data();
            job.indexCount = (uint32_t)data->indices.size();
            job.placeholder = (uint32_t)(jobs.size() / batchJobs) * 2;
            job.state = 0;
            jobs.push_back(job);
        }
    }
};

bool Same(const GfxBatchData &a, const GfxBatchData &b)
{
    if (a.draws.size() != b.draws.size() || a.vertices.size() != b.vertices.size() || a.drawIndices != b.drawIndices)
        return false;
    for (size_t i = 0; i < a.draws.size(); i++) {
        const GfxCommand &x = a.draws[i].draw, &y = b.draws[i].draw;
        if (a.draws[i].placeholder != b.draws[i].placeholder || x.vertexCount != y.vertexCount ||
            x.indexCount != y.indexCount || x.firstVertexByte != y.firstVertexByte || x.firstIndex != y.firstIndex)
            return false;
    }
    return a.vertices.empty() ||
           memcmp(a.vertices.data(), b.vertices.data(), a.vertices.size() * sizeof(GfxVertex)) == 0;
}

} // namespace

int main()
{
    GfxVertexBuffer buffer;
    buffer.vertices.resize(12);
    for (int i = 0; i < 12; i++) {
        buffer.vertices[i].x = (float)i;
        buffer.vertices[i].y = (float)(i % 3);
    }

    // Without the thread: a job runs when it is added.
    Batches inPlace(buffer, 1000, 7, 3);
    REQUIRE(Gfx_BatchJobsAdded() == 0 && Gfx_BatchJobsDone() == 0);
    uint32_t number = 0;
    for (const GfxBatchJob &job : inPlace.jobs) {
        REQUIRE(Gfx_AddBatchJob(job) == ++number);
        REQUIRE(Gfx_BatchJobsAdded() == number && Gfx_BatchJobsDone() == number);
    }
    Gfx_WaitForBatchJobs(number, 0);
    // The jobs of a batch join their draws: 3 jobs of 7 triangles are one
    // draw of 63 vertices, with the vertices moved by 2 on x.
    REQUIRE(inPlace.data->draws.size() == (inPlace.jobs.size() + 2) / 3);
    REQUIRE(inPlace.data->draws[0].draw.vertexCount == 63 && inPlace.data->draws[0].placeholder == 0);
    REQUIRE(inPlace.data->draws[1].placeholder == 2 && inPlace.data->vertices.size() == 3000);
    REQUIRE(inPlace.data->vertices[4].x == 6.0f && inPlace.data->vertices[4].y == 1.0f);

    // With the thread: the same draws.
    REQUIRE(Gfx_StartBatchThread());
    Batches onThread(buffer, 1000, 7, 3);
    for (const GfxBatchJob &job : onThread.jobs)
        REQUIRE(Gfx_AddBatchJob(job) == ++number);
    Gfx_WaitForBatchJobs(number, 0);
    REQUIRE(Gfx_BatchJobsDone() == number && Same(*inPlace.data, *onThread.data));

    // More jobs than the queue holds (256): each job of one triangle, each a
    // batch of its own. The caller waits for room, and the order stays.
    Batches manyInPlace(buffer, 1500, 1, 1), manyOnThread(buffer, 1500, 1, 1);
    Gfx_StopBatchThread();
    for (const GfxBatchJob &job : manyInPlace.jobs)
        number = Gfx_AddBatchJob(job);
    REQUIRE(Gfx_StartBatchThread());
    for (const GfxBatchJob &job : manyOnThread.jobs)
        number = Gfx_AddBatchJob(job);
    Gfx_WaitForBatchJobs(number, 1);
    REQUIRE(manyOnThread.data->draws.size() == 1500 && Same(*manyInPlace.data, *manyOnThread.data));

    // A wait for a job returns when it is done: the caller then writes the
    // vertices, and no draw of the job has what it writes. 200,000
    // triangles in one job, so that the job runs for some time.
    {
        Batches large(buffer, 200000, 200000, 1);
        number = Gfx_AddBatchJob(large.jobs[0]);
        REQUIRE(Gfx_BatchJobsAdded() == number);
        Gfx_WaitForBatchJobs(number, 0);
        for (GfxVertex &v : buffer.vertices)
            v.x = 100.0f;
        REQUIRE(Gfx_BatchJobsDone() == number && large.data->vertices.size() == 600000);
        double sum = 0.0;
        for (const GfxVertex &v : large.data->vertices)
            sum += v.x;
        // Each triangle i has the vertices (i % 4) * 3 .. + 2, moved by 2.
        REQUIRE(sum == 50000.0 * (3 + 12 + 21 + 30) + 600000.0 * 2.0);
        const GfxBatchTimes times = Gfx_TakeBatchTimes();
        REQUIRE(times.jobs > 0);
    }

    // After the stop a job runs when it is added again.
    Gfx_StopBatchThread();
    Batches last(buffer, 3, 3, 1);
    REQUIRE(Gfx_AddBatchJob(last.jobs[0]) == number + 1 && Gfx_BatchJobsDone() == number + 1);
    REQUIRE(last.data->vertices.size() == 9 && last.data->vertices[0].x == 102.0f);

    printf("gfx_batch_worker: ok\n");
    return 0;
}
