// The threaded executor (src/render/gfx_threaded.cpp): the frames run on a
// worker thread, one at a time and in their order, while the caller goes on.
// Prepare runs on the caller's thread before its frame. Each other call
// waits for the worker: no call of the inner executor meets a running
// frame. The small batches of a frame become draws on the worker.

#include "render/gfx_internal.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "failed line %d: %s\n", __LINE__, #x); return 1; } } while (0)

extern "C" void Sys_Log(const char *, ...) {}

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
    double vertexSum = 0.0;                 // of the last frame: see Execute
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
    void Execute(GfxFrame &frame, std::vector<GfxTexture *> &, GfxReadback &, bool, bool, bool) override
    {
        Check();
        running = true;
        if (std::this_thread::get_id() == caller)
            executedOnCaller++;
        for (const GfxCommand &c : frame.commands)
            batchesSeen += c.type == GFX_CMD_SMALL_BATCH ? 1 : 0;
        // The x of the second vertex of each triangle of the frame's data.
        double sum = 0.0;
        for (size_t at = sizeof(GfxVertex); at + sizeof(GfxVertex) <= frame.vertices.size(); at += 3 * sizeof(GfxVertex)) {
            GfxVertex v;
            memcpy(&v, frame.vertices.data() + at, sizeof(v));
            sum += v.x;
        }
        vertexSum = sum;
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
    bool JoinsDraws() const override { return true; }
    void ReadTexture(GfxTexture *) override { Check(); }
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

    // A small batch becomes draws on the worker: Execute gets none.
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
    frame.smallItems.push_back(item);
    frame.smallIndices = { 0, 1, 2 };
    threaded->Execute(frame, textures, readback, true, false, false);
    threaded->Wait();
    REQUIRE(slow->batchesSeen == 0 && slow->commandCounts.size() == 7 && slow->commandCounts[6] == 1);

    // The worker reads the vertices of the small batches from their vertex
    // buffer. WaitForBatches returns when it has read them, before the 30
    // ms of the frame end: the caller then writes the vertices, and each
    // draw of the frame has them as they were. 60,000 draws, so that the
    // worker reads for some time.
    {
        const int kItems = 60000;
        triangle.vertices[1].x = 1.0f;
        frame = FrameOf(0);
        batch.itemCount = kItems;
        frame.commands.push_back(batch);
        frame.smallItems.assign(kItems, item);
        frame.smallIndices.clear();
        for (int i = 0; i < kItems; i++) {
            frame.smallItems[i].firstIndex = (uint32_t)frame.smallIndices.size();
            frame.smallIndices.insert(frame.smallIndices.end(), { 0, 1, 2 });
        }
        const int finishedBefore = slow->finished;
        slow->vertexSum = 0.0;
        threaded->Execute(frame, textures, readback, false, false, false);
        threaded->WaitForBatches();
        triangle.vertices[1].x = 5.0f;
        REQUIRE(slow->finished == finishedBefore);
        threaded->Wait();
        // Each draw has x = 1 + 2 (the world matrix moves it by 2).
        REQUIRE(slow->finished == finishedBefore + 1 && slow->vertexSum == 3.0 * kItems);
        threaded->WaitForBatches();     // an idle worker: at once
    }

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
