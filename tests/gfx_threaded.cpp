// The threaded executor (src/render/gfx_threaded.cpp): the frames run on a
// worker thread, one at a time and in their order, while the caller goes on.
// Prepare runs on the caller's thread before its frame. Each other call
// waits for the worker: no call of the inner executor meets a running
// frame. The draws of the small batches of a frame go in their place on the
// worker.

#include "render/gfx_batch_worker.h"
#include "render/gfx_internal.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "failed line %d: %s\n", __LINE__, #x); return 1; } } while (0)

extern "C" void Sys_Log(const char *, ...) {}
extern "C" int Sys_GetOption(const char *, int fallback) { return fallback; }

namespace {

// An executor whose Execute takes some time, and which notes each call that
// meets a running Execute, and where each call runs.
struct SlowExecutor : GfxExecutor {
    std::atomic<bool> running{ false };
    std::atomic<int> overlaps{ 0 };
    std::thread::id caller = std::this_thread::get_id();
    std::atomic<int> executedOnCaller{ 0 }, preparedElsewhere{ 0 };
    std::vector<size_t> commandCounts;      // of each frame, in the order they ran
    std::vector<size_t> preparedCounts;     // of each frame Prepare saw
    std::atomic<int> batchesSeen{ 0 };      // GFX_CMD_SMALL_BATCH commands that reached Execute
    std::atomic<int> destroyed{ 0 };
    std::atomic<int> finished{ 0 };         // frames whose Execute ended
    GfxWorkerTimes noted;                   // us, the sums of NoteWorkerTimes
    std::vector<int> notesBeforePrepare;    // the NoteWorkerTimes calls before each Prepare
    int notes = 0;
    // The threaded executor: Execute calls its Wait, as a system dialog does
    // from the GXM executor (Gfx_WaitForExecutor). It returns at once there.
    GfxExecutor *owner = nullptr;

    // Prepare takes some time too: a Prepare that ran with the worker would
    // meet its Execute.
    std::atomic<bool> preparing{ false };

    void Check() { overlaps += running || preparing ? 1 : 0; }

    bool SetVideoMode(int, int, bool) override
    {
        Check();
        return true;
    }
    void SetFullscreen(bool) override { Check(); }
    void Prepare(GfxFrame &frame, std::vector<GfxTexture *> &, GfxReadback &, bool) override
    {
        Check();
        preparing = true;
        if (std::this_thread::get_id() != caller)
            preparedElsewhere++;
        preparedCounts.push_back(frame.commands.size());
        notesBeforePrepare.push_back(notes);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        preparing = false;
    }
    void NoteWorkerTimes(const GfxWorkerTimes &times) override
    {
        Check();
        noted.presentWait += times.presentWait;
        noted.flushWait += times.flushWait;
        noted.otherWait += times.otherWait;
        notes++;
    }
    void Execute(GfxFrame &frame, std::vector<GfxTexture *> &, GfxReadback &, bool, int, bool) override
    {
        Check();
        running = true;
        if (std::this_thread::get_id() == caller)
            executedOnCaller++;
        for (const GfxCommand &c : frame.commands)
            batchesSeen += c.type == GFX_CMD_SMALL_BATCH ? 1 : 0;
        if (owner != nullptr)
            owner->Wait();
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        commandCounts.push_back(frame.commands.size());
        finished++;
        running = false;
    }
    void DestroyTexture(GfxTexture *) override
    {
        Check();
        destroyed++;
    }
    void DestroyVertexBuffer(GfxVertexBuffer *) override { Check(); }
    // The snapshots (SetSnapshotCopiesLate): copied on the worker before the
    // frame's Execute, some time after the caller went on.
    std::atomic<bool> late{ false };
    std::atomic<int> snapshotCopies{ 0 }, copiesOnCaller{ 0 }, copiesInExecute{ 0 };
    void SetSnapshotCopiesLate(bool value) override { late = value; }
    void CopySnapshots(GfxFrame &) override
    {
        if (std::this_thread::get_id() == caller)
            copiesOnCaller++;
        copiesInExecute += running ? 1 : 0;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        snapshotCopies++;
    }
    bool JoinsDraws() const override { return true; }
    void ReadTexture(GfxTexture *) override { Check(); }
    // The frames of a video decoder: each call must meet no running frame.
    std::atomic<int> videoCalls{ 0 };
    bool SetTextureVideoFrame(GfxTexture *, const void *, bool, void *, size_t) override
    {
        Check();
        videoCalls++;
        return true;
    }
    void FinishDrawing() override
    {
        Check();
        videoCalls++;
    }
    void ReleaseVideoMemory(void *) override
    {
        Check();
        videoCalls++;
    }
};

// Waits until the worker runs a frame (at most a second): a call after it
// meets that frame unless it waits.
void UntilRunning(const SlowExecutor &slow)
{
    for (int i = 0; i < 1000 && !slow.running; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

GfxFrame FrameOf(size_t draws)
{
    GfxFrame frame;
    frame.states.resize(1);
    for (size_t i = 0; i < draws; i++) {
        GfxCommand c = {};
        c.type = GFX_CMD_CLEAR;
        frame.commands.push_back(c);
    }
    return frame;
}

} // namespace

int main()
{
    SlowExecutor *slow = new SlowExecutor;
    GfxExecutor *threaded = Gfx_CreateThreadedExecutor(slow);
    REQUIRE(threaded != slow);
    slow->owner = threaded;
    std::vector<GfxTexture *> textures;
    GfxReadback readback;

    // A frame runs on the worker: the call returns after Prepare and before
    // the 30 ms of Execute end (no frame has ended), and the caller's frame
    // has the contents the worker ran before (none).
    GfxFrame frame = FrameOf(3);
    threaded->Prepare(frame, textures, readback, true);
    threaded->Execute(frame, textures, readback, true, false, false);
    REQUIRE(slow->finished == 0);
    REQUIRE(frame.commands.empty());
    REQUIRE(slow->preparedCounts.size() == 1 && slow->preparedCounts[0] == 3);

    // The next frame waits for it: the frames run in their order, one at a
    // time. A texture that goes waits for the frame that runs. The wait (most
    // of the 30 ms of the first frame) goes to the inner executor before the
    // Prepare of the frame.
    REQUIRE(slow->noted.presentWait < 5000);
    frame = FrameOf(5);
    threaded->Execute(frame, textures, readback, true, false, false);
    REQUIRE(slow->noted.presentWait >= 20000 && slow->noted.flushWait == 0 && slow->noted.otherWait == 0);
    REQUIRE(slow->notesBeforePrepare.size() == 2 && slow->notesBeforePrepare[1] == 2);
    UntilRunning(*slow);
    threaded->DestroyTexture(nullptr);
    frame = FrameOf(2);
    threaded->Execute(frame, textures, readback, false, false, false);
    UntilRunning(*slow);
    threaded->ReadTexture(nullptr);
    frame = FrameOf(4);
    threaded->Execute(frame, textures, readback, false, false, false);
    UntilRunning(*slow);
    threaded->DestroyVertexBuffer(nullptr);
    frame = FrameOf(6);
    threaded->Execute(frame, textures, readback, false, false, false);
    UntilRunning(*slow);
    REQUIRE(threaded->SetVideoMode(640, 480, false));
    frame = FrameOf(7);
    threaded->Execute(frame, textures, readback, false, false, false);
    UntilRunning(*slow);
    threaded->SetFullscreen(false);
    threaded->Wait();
    const size_t order[6] = { 3, 5, 2, 4, 6, 7 };
    REQUIRE(slow->commandCounts.size() == 6 && slow->preparedCounts.size() == 6);
    for (int i = 0; i < 6; i++)
        REQUIRE(slow->commandCounts[i] == order[i] && slow->preparedCounts[i] == order[i]);
    REQUIRE(slow->overlaps == 0 && slow->destroyed == 1);
    REQUIRE(slow->executedOnCaller == 0 && slow->preparedElsewhere == 0);

    // A small batch whose jobs are done becomes draws on the worker, which
    // puts them in the place of the batch: Execute gets no batch.
    frame = FrameOf(0);
    GfxCommand batch = {};
    batch.type = GFX_CMD_SMALL_BATCH;
    batch.state = 0;
    batch.firstItem = 0;
    batch.itemCount = 1;
    frame.commands.push_back(batch);
    GfxSmallItem item = {};
    item.world._11 = item.world._22 = item.world._33 = item.world._44 = 1.0f;
    item.world._41 = 2.0f;
    item.vertexCount = 3;
    item.indexCount = 3;
    GfxVertexBuffer triangle;
    triangle.vertices.resize(3);
    item.buffer = &triangle;
    frame.batch = std::make_shared<GfxBatchData>();
    frame.batch->items.push_back(item);
    frame.batch->indices = { 0, 1, 2 };
    GfxBatchJob job = {};
    job.data = frame.batch.get();
    job.items = frame.batch->items.data();
    job.itemCount = 1;
    job.indices = frame.batch->indices.data();
    job.indexCount = 3;
    job.placeholder = 0;
    job.state = 0;
    frame.batchSequence = Gfx_AddBatchJob(job);
    threaded->Execute(frame, textures, readback, true, false, false);
    threaded->Wait();
    REQUIRE(slow->batchesSeen == 0 && slow->commandCounts.size() == 7 && slow->commandCounts[6] == 1);

    // An Execute in the middle of a frame that waits for the one before:
    // the wait is that of a flush.
    const unsigned flushBefore = slow->noted.flushWait;
    frame = FrameOf(1);
    threaded->Execute(frame, textures, readback, false, false, false);
    frame = FrameOf(1);
    threaded->Execute(frame, textures, readback, false, false, false);
    REQUIRE(slow->noted.flushWait - flushBefore >= 20000);
    // The wait of another call, right after an Execute (most of its 30 ms),
    // goes with the next Execute, apart from the waits of the Execute calls.
    const unsigned otherBefore = slow->noted.otherWait;
    threaded->DestroyTexture(nullptr);
    frame = FrameOf(1);
    threaded->Execute(frame, textures, readback, false, false, false);
    REQUIRE(slow->noted.otherWait - otherBefore >= 20000);

    // The calls for the frames of a video decoder wait for the frame that
    // runs, and reach the inner executor.
    frame = FrameOf(1);
    threaded->Execute(frame, textures, readback, false, false, false);
    UntilRunning(*slow);
    REQUIRE(threaded->SetTextureVideoFrame(nullptr, nullptr, true, nullptr, 0));
    frame = FrameOf(1);
    threaded->Execute(frame, textures, readback, false, false, false);
    UntilRunning(*slow);
    threaded->FinishDrawing();
    frame = FrameOf(1);
    threaded->Execute(frame, textures, readback, false, false, false);
    UntilRunning(*slow);
    threaded->ReleaseVideoMemory(nullptr);
    REQUIRE(slow->videoCalls == 3);

    // The snapshots: the worker copies them first in its job; a wait for
    // them returns after that copy and before the frame ends.
    REQUIRE(slow->late && threaded->CopiesSnapshotsLate());
    {
        const int copiesBefore = slow->snapshotCopies, finishedBefore = slow->finished;
        frame = FrameOf(1);
        threaded->Execute(frame, textures, readback, false, false, false);
        threaded->WaitForSnapshotCopies();
        REQUIRE(slow->snapshotCopies == copiesBefore + 1 && slow->finished == finishedBefore);
        REQUIRE(slow->copiesOnCaller == 0 && slow->copiesInExecute == 0);
        // Another wait for the same frame returns at once.
        threaded->WaitForSnapshotCopies();
        threaded->Wait();
        REQUIRE(slow->finished == finishedBefore + 1);
    }

    // Each Execute called the wait of its own worker, which returned (else
    // the first frame would never have ended). A frame that runs when the
    // threaded executor goes ends first; the inner executor goes with it.
    REQUIRE(slow->overlaps == 0);
    frame = FrameOf(1);
    threaded->Execute(frame, textures, readback, true, false, false);
    delete threaded;
    printf("gfx_threaded: ok\n");
    return 0;
}
