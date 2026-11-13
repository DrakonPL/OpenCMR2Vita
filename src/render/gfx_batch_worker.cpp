// The batch jobs on a thread of their own (gfx_batch_worker.h).

#include "render/gfx_batch_worker.h"

#include "port/sys.h"

#include <atomic>

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

// The jobs that wait: a ring. A frame of a wood has some 80 jobs.
constexpr uint32_t kCapacity = 256;
GfxBatchJob s_ring[kCapacity];
// The numbers of the last job added and of the last job done. The thread
// that adds writes s_added, the batch thread s_done.
std::atomic<uint32_t> s_added{ 0 }, s_done{ 0 };
std::atomic<uint32_t> s_jobMicros{ 0 }, s_waitMicros[2] = { { 0 }, { 0 } };
bool s_onThread;    // the jobs run on the batch thread

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

void RunTimed(const GfxBatchJob &job)
{
    const unsigned long long start = Micros();
    Gfx_RunBatchJob(job);
    s_jobMicros.fetch_add((uint32_t)(Micros() - start));
}

// The next job of the ring, on the batch thread.
void RunNext()
{
    const uint32_t done = s_done.load();
    RunTimed(s_ring[done % kCapacity]);
    s_done.store(done + 1);
}

#ifdef VITA

constexpr int kPriority = 140;                  // below the sound (96, 128), which has core 1 too
constexpr SceSize kStackBytes = 64 * 1024;      // a job has 16 vertices and a matrix on its stack
SceUID s_thread = -1, s_wake = -1;
volatile bool s_quit;

int Entry(SceSize, void *)
{
    for (;;) {
        sceKernelWaitSema(s_wake, 1, nullptr);
        if (s_quit)
            return 0;
        RunNext();
    }
}

bool StartThread()
{
    s_wake = sceKernelCreateSema("gfx_batch", 0, 0, (int)kCapacity + 1, nullptr);
    s_quit = false;
    s_thread = s_wake < 0 ? -1
                          : sceKernelCreateThread("gfx_batch", Entry, kPriority, kStackBytes, 0,
                                                  SCE_KERNEL_CPU_MASK_USER_1, nullptr);
    const int started = s_thread < 0 ? s_thread : sceKernelStartThread(s_thread, 0, nullptr);
    if (started < 0) {
        Sys_Log("gfx: no batch thread (sema %08x, thread %08x, start %08x): the batches run on the game's thread",
                (unsigned)s_wake, (unsigned)s_thread, (unsigned)started);
        if (s_thread >= 0)
            sceKernelDeleteThread(s_thread);
        if (s_wake >= 0)
            sceKernelDeleteSema(s_wake);
        s_thread = s_wake = -1;
        return false;
    }
    return true;
}

void StopThread()
{
    s_quit = true;
    sceKernelSignalSema(s_wake, 1);
    sceKernelWaitThreadEnd(s_thread, nullptr, nullptr);
    sceKernelDeleteThread(s_thread);
    sceKernelDeleteSema(s_wake);
    s_thread = s_wake = -1;
}

void Wake()
{
    sceKernelSignalSema(s_wake, 1);
}

// A short rest for a thread that waits for the batch thread, which has a
// core of its own.
void Rest()
{
    sceKernelDelayThread(50);
}

#else

std::mutex s_mutex;
std::condition_variable s_wake;
uint32_t s_signals;     // jobs added and not taken yet, and 1 for the stop
bool s_quit;
std::thread s_thread;

void Entry()
{
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(s_mutex);
            s_wake.wait(lock, [] { return s_signals > 0; });
            s_signals--;
            if (s_quit)
                return;
        }
        RunNext();
    }
}

bool StartThread()
{
    s_quit = false;
    s_signals = 0;
    s_thread = std::thread(Entry);
    return true;
}

void StopThread()
{
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_quit = true;
        s_signals++;
    }
    s_wake.notify_all();
    s_thread.join();
}

void Wake()
{
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_signals++;
    }
    s_wake.notify_all();
}

void Rest()
{
    std::this_thread::yield();
}

#endif

// Whether the job of that number is done (the numbers wrap).
bool Done(uint32_t number)
{
    return (int32_t)(s_done.load() - number) >= 0;
}

} // namespace

bool Gfx_StartBatchThread(void)
{
    if (!s_onThread)
        s_onThread = StartThread();
    return s_onThread;
}

void Gfx_StopBatchThread(void)
{
    if (!s_onThread)
        return;
    Gfx_WaitForBatchJobs(s_added.load(), 0);
    StopThread();
    s_onThread = false;
}

uint32_t Gfx_AddBatchJob(const GfxBatchJob &job)
{
    const uint32_t number = s_added.load() + 1;
    if (!s_onThread) {
        RunTimed(job);
        s_added.store(number);
        s_done.store(number);
        return number;
    }
    // A full ring: the batch thread is behind by all of it.
    if (number - s_done.load() > kCapacity)
        Gfx_WaitForBatchJobs(number - kCapacity, 0);
    s_ring[(number - 1) % kCapacity] = job;
    s_added.store(number);
    Wake();
    return number;
}

uint32_t Gfx_BatchJobsAdded(void)
{
    return s_added.load();
}

uint32_t Gfx_BatchJobsDone(void)
{
    return s_done.load();
}

void Gfx_WaitForBatchJobs(uint32_t number, int waiter)
{
    if (Done(number))
        return;
    const unsigned long long start = Micros();
    while (!Done(number))
        Rest();
    s_waitMicros[waiter != 0].fetch_add((uint32_t)(Micros() - start));
}

GfxBatchTimes Gfx_TakeBatchTimes(void)
{
    GfxBatchTimes times;
    times.jobs = s_jobMicros.exchange(0);
    times.waited[0] = s_waitMicros[0].exchange(0);
    times.waited[1] = s_waitMicros[1].exchange(0);
    return times;
}
