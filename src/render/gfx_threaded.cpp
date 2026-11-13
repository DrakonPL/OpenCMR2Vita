// The threaded executor (gfx_internal.h, Gfx_CreateThreadedExecutor): runs
// another executor's Execute on a worker thread, while the game records the
// next frame. On the PS Vita the recorder and the game take one core; the
// draw calls of the GXM executor took 6 to 10 ms of each frame on it.
//
// The rules that make it safe:
//  - The worker runs one frame at a time. The recorder hands it the next
//    frame only when it is idle (Wait), so a frame is never two frames late.
//  - The worker reads only its own frame (the recorder's frame changes place
//    with it). What reads the recorder's objects (the textures, the vertex
//    buffers, the readback) is the inner executor's Prepare, which runs on
//    the recorder's thread while the worker is idle.
//  - Each other call (a video mode, a texture or vertex buffer that goes, a
//    read of a render target) waits for the worker first.
//  - The small batches of the frame become draws on the worker
//    (Gfx_ExpandSmallBatches): the recorder only copies them.

#include "render/gfx_internal.h"

#include "port/sys.h"

#include <memory>
#include <utility>
#include <vector>

#ifdef VITA
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#else
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#endif

namespace {

// A clock in us, for the logs of the executor.
unsigned long long Micros()
{
#ifdef VITA
    return sceKernelGetProcessTimeWide();
#else
    return (unsigned long long)std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
#endif
}

// A thread that runs one job at a time: Start hands it the job, Wait waits
// until it is done. Only the thread that made it calls Start and Wait.
class Worker {
public:
    typedef void (*Job)(void *);

#ifdef VITA
    explicit Worker(Job job, void *argument) : job(job), argument(argument)
    {
        start = sceKernelCreateSema("gfx_start", 0, 0, 1, nullptr);
        done = sceKernelCreateSema("gfx_done", 0, 0, 1, nullptr);
        // Core 0 runs the game: the worker has a core of its own.
        thread = sceKernelCreateThread("gfx_worker", Entry, kPriority, kStackBytes, 0, SCE_KERNEL_CPU_MASK_USER_2,
                                       nullptr);
        Worker *self = this;
        const int started = thread < 0 ? thread : sceKernelStartThread(thread, sizeof(self), &self);
        if (start < 0 || done < 0 || thread < 0 || started < 0) {
            Sys_Log("gfx: no worker thread (sema %08x %08x, thread %08x, start %08x): the frames run on the game's "
                    "thread",
                    (unsigned)start, (unsigned)done, (unsigned)thread, (unsigned)started);
            if (thread >= 0)
                sceKernelDeleteThread(thread);
            thread = -1;
            return;
        }
        ok = true;
    }
    ~Worker()
    {
        if (ok) {
            Wait();
            quit = true;
            sceKernelSignalSema(start, 1);
            sceKernelWaitThreadEnd(thread, nullptr, nullptr);
            sceKernelDeleteThread(thread);
        }
        if (start >= 0)
            sceKernelDeleteSema(start);
        if (done >= 0)
            sceKernelDeleteSema(done);
    }
    void Start()
    {
        busy = true;
        sceKernelSignalSema(start, 1);
    }
    void Wait()
    {
        if (busy)
            sceKernelWaitSema(done, 1, nullptr);
        busy = false;
    }
    bool OnWorker() const { return ok && sceKernelGetThreadId() == threadId; }
#else
    explicit Worker(Job job, void *argument) : job(job), argument(argument), thread([this] { Run(); })
    {
        ok = true;
    }
    ~Worker()
    {
        Wait();
        {
            std::lock_guard<std::mutex> lock(mutex);
            quit = true;
        }
        wake.notify_all();
        thread.join();
    }
    void Start()
    {
        busy = true;
        {
            std::lock_guard<std::mutex> lock(mutex);
            pending = true;
        }
        wake.notify_all();
    }
    void Wait()
    {
        if (!busy)
            return;
        std::unique_lock<std::mutex> lock(mutex);
        finished.wait(lock, [this] { return !pending; });
        busy = false;
    }
    bool OnWorker() const { return std::this_thread::get_id() == thread.get_id(); }
#endif

    bool Ok() const { return ok; }

private:
    Job job;
    void *argument;
    bool ok = false;
    bool busy = false;      // the making thread only: a job was started and not waited for
#ifdef VITA
    static constexpr int kPriority = 0x10000100;    // the SDK's default for user threads
    static constexpr SceSize kStackBytes = 256 * 1024;
    static int Entry(SceSize, void *data)
    {
        Worker *self = *(Worker **)data;
        self->threadId = sceKernelGetThreadId();
        for (;;) {
            sceKernelWaitSema(self->start, 1, nullptr);
            if (self->quit)
                return 0;
            self->job(self->argument);
            sceKernelSignalSema(self->done, 1);
        }
    }
    SceUID start = -1, done = -1, thread = -1;
    volatile SceUID threadId = -1;
    volatile bool quit = false;
#else
    void Run()
    {
        std::unique_lock<std::mutex> lock(mutex);
        for (;;) {
            wake.wait(lock, [this] { return pending || quit; });
            if (quit)
                return;
            lock.unlock();
            job(argument);
            lock.lock();
            pending = false;
            finished.notify_all();
        }
    }
    std::mutex mutex;
    std::condition_variable wake, finished;
    bool pending = false, quit = false;
    std::thread thread;     // last: it starts when the rest is ready
#endif
};

struct ThreadedExecutor : GfxExecutor {
    std::unique_ptr<GfxExecutor> inner;
    // The frame the worker runs, and how.
    GfxFrame frame;
    bool present = false, vsync = false, stretch = false;
    // The worker's Execute gets no textures and no readback: Prepare took
    // what it needs of them.
    std::vector<GfxTexture *> noTextures;
    GfxReadback noReadback;
    // For the logs (GfxExecutor::NoteWorkerTimes): the us the game's thread
    // waited, and the worker's expand of the last frame.
    unsigned long long waitMicros = 0;
    unsigned expandMicros = 0;
    Worker worker;

    explicit ThreadedExecutor(GfxExecutor *executor) : inner(executor), worker(&Run, this) {}
    ~ThreadedExecutor() override { worker.Wait(); }

    static void Run(void *data)
    {
        ThreadedExecutor *self = (ThreadedExecutor *)data;
        const unsigned long long start = Micros();
        Gfx_ExpandSmallBatches(self->frame);
        self->expandMicros = (unsigned)(Micros() - start);
        self->inner->Execute(self->frame, self->noTextures, self->noReadback, self->present, self->vsync,
                             self->stretch);
    }

    void Wait() override
    {
        if (worker.OnWorker())
            return;
        const unsigned long long start = Micros();
        worker.Wait();
        waitMicros += Micros() - start;
    }

    bool SetVideoMode(int width, int height, bool fullscreen) override
    {
        Wait();
        return inner->SetVideoMode(width, height, fullscreen);
    }
    void SetFullscreen(bool fullscreen) override
    {
        Wait();
        inner->SetFullscreen(fullscreen);
    }
    // The recorder's frame changes place with the frame the worker ran: the
    // recorder resets it after this call.
    void Execute(GfxFrame &recorded, std::vector<GfxTexture *> &textures, GfxReadback &readback, bool presentFrame,
                 bool vsyncFrame, bool stretchFrame) override
    {
        Wait();
        inner->NoteWorkerTimes((unsigned)waitMicros, expandMicros);
        waitMicros = 0;
        expandMicros = 0;
        std::swap(frame, recorded);
        inner->Prepare(frame, textures, readback, presentFrame);
        present = presentFrame;
        vsync = vsyncFrame;
        stretch = stretchFrame;
        worker.Start();
    }
    void DestroyTexture(GfxTexture *texture) override
    {
        Wait();
        inner->DestroyTexture(texture);
    }
    void DestroyVertexBuffer(GfxVertexBuffer *buffer) override
    {
        Wait();
        inner->DestroyVertexBuffer(buffer);
    }
    bool JoinsDraws() const override { return inner->JoinsDraws(); }
    void ReadTexture(GfxTexture *texture) override
    {
        Wait();
        inner->ReadTexture(texture);
    }
    // The readback is taken in Prepare, with the worker idle.
    void CollectReadback(GfxReadback &) override {}
};

} // namespace

GfxExecutor *Gfx_CreateThreadedExecutor(GfxExecutor *inner)
{
    ThreadedExecutor *threaded = new ThreadedExecutor(inner);
    if (threaded->worker.Ok())
        return threaded;
    threaded->inner.release();
    delete threaded;
    return inner;
}
