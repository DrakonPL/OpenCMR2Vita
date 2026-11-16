// The game's jobs on a thread of their own (port/game_jobs.h). The batch
// thread of the recorder (gfx_batch_worker.cpp) is not used: it runs the
// tree jobs of the last frame into the next one, and a game job would wait
// behind them.

#include "port/game_jobs.h"

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

// The jobs that wait: a ring. A frame has one or two jobs for each view.
constexpr uint32_t kCapacity = 8;
struct Job {
    GameJobRun run;
    void *context;
};
Job s_ring[kCapacity];
// The numbers of the last job added and of the last job done. The adding
// thread writes s_added, the job thread s_done.
std::atomic<uint32_t> s_added{ 0 }, s_done{ 0 };
std::atomic<uint32_t> s_jobMicros{ 0 }, s_waitMicros{ 0 }, s_checks{ 0 }, s_mismatches{ 0 };
// -1 until GameJobs_Start or the first GameJobs_Mode.
int s_mode = -1;
bool s_onThread;

// A log line for each of the first mismatches, then one for each this many.
constexpr uint32_t kLoggedMismatches = 8;
constexpr uint32_t kChecksPerSummary = 1000;

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

void SignalDone();

// The next job of the ring, on the job thread.
void RunNext()
{
    const uint32_t done = s_done.load();
    const Job job = s_ring[done % kCapacity];
    const unsigned long long start = Micros();
    job.run(job.context);
    s_jobMicros.fetch_add((uint32_t)(Micros() - start));
    s_done.store(done + 1);
    SignalDone();
}

bool Done(uint32_t number)
{
    return (int32_t)(s_done.load() - number) >= 0;
}

#ifdef VITA

constexpr int kPriority = 134;                  // between the music (128) and the batch thread (140), all on core 1
constexpr SceSize kStackBytes = 64 * 1024;
SceUID s_thread = -1, s_wake = -1, s_doneSignal = -1;
volatile bool s_quit;
// Set while the adding thread waits on s_doneSignal.
std::atomic<int> s_waiting{ 0 };
// The FPSCR of each thread: the float steps of a job give the bits of the
// game's thread only with the same rounding and flush-to-zero.
uint32_t s_gameFpscr;
std::atomic<uint32_t> s_jobFpscr{ 0 };
std::atomic<int> s_jobFpscrRead{ 0 };
bool s_fpscrLogged;

uint32_t Fpscr()
{
    uint32_t value;
    __asm__ volatile("vmrs %0, fpscr" : "=r"(value));
    return value;
}

int Entry(SceSize, void *)
{
    s_jobFpscr.store(Fpscr());
    s_jobFpscrRead.store(1);
    for (;;) {
        sceKernelWaitSema(s_wake, 1, nullptr);
        if (s_quit)
            return 0;
        RunNext();
    }
}

void DeleteKernelObjects()
{
    if (s_thread >= 0)
        sceKernelDeleteThread(s_thread);
    if (s_wake >= 0)
        sceKernelDeleteSema(s_wake);
    if (s_doneSignal >= 0)
        sceKernelDeleteSema(s_doneSignal);
    s_thread = s_wake = s_doneSignal = -1;
}

bool StartThread()
{
    s_wake = sceKernelCreateSema("game_jobs", 0, 0, (int)kCapacity + 1, nullptr);
    s_doneSignal = sceKernelCreateSema("game_jobs_done", 0, 0, (int)kCapacity + 1, nullptr);
    s_quit = false;
    s_gameFpscr = Fpscr();
    s_thread = s_wake < 0 || s_doneSignal < 0
                   ? -1
                   : sceKernelCreateThread("game_jobs", Entry, kPriority, kStackBytes, 0, SCE_KERNEL_CPU_MASK_USER_1,
                                           nullptr);
    const int started = s_thread < 0 ? s_thread : sceKernelStartThread(s_thread, 0, nullptr);
    if (started < 0) {
        Sys_Log("game jobs: no thread (semas %08x %08x, thread %08x, start %08x): the jobs run inline",
                (unsigned)s_wake, (unsigned)s_doneSignal, (unsigned)s_thread, (unsigned)started);
        DeleteKernelObjects();
        return false;
    }
    return true;
}

void StopThread()
{
    s_quit = true;
    sceKernelSignalSema(s_wake, 1);
    sceKernelWaitThreadEnd(s_thread, nullptr, nullptr);
    DeleteKernelObjects();
}

void Wake()
{
    sceKernelSignalSema(s_wake, 1);
}

void SignalDone()
{
    if (s_waiting.load())
        sceKernelSignalSema(s_doneSignal, 1);
}

void Block(uint32_t number)
{
    // A signal of an earlier wait that found its job done.
    while (sceKernelPollSema(s_doneSignal, 1) >= 0) {
    }
    s_waiting.store(1);
    while (!Done(number))
        sceKernelWaitSema(s_doneSignal, 1, nullptr);
    s_waiting.store(0);
}

void LogFpscrOnce()
{
    if (s_fpscrLogged || !s_jobFpscrRead.load())
        return;
    s_fpscrLogged = true;
    const uint32_t job = s_jobFpscr.load();
    Sys_Log("game jobs: FPSCR game thread %08x, job thread %08x: %s", (unsigned)s_gameFpscr, (unsigned)job,
            s_gameFpscr == job ? "same" : "DIFFERENT, a float step of a job can give other bits");
}

#else

std::mutex s_mutex;
std::condition_variable s_wake, s_doneSignal;
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

void SignalDone()
{
    // The lock orders the store of s_done before the check of a waiter.
    { std::lock_guard<std::mutex> lock(s_mutex); }
    s_doneSignal.notify_all();
}

void Block(uint32_t number)
{
    std::unique_lock<std::mutex> lock(s_mutex);
    s_doneSignal.wait(lock, [number] { return Done(number); });
}

void LogFpscrOnce()
{
}

// A thread that runs at the exit of the process ends the process.
struct StopAtExit {
    ~StopAtExit() { GameJobs_Stop(); }
} s_stopAtExit;

#endif

} // namespace

void GameJobs_Start(int mode)
{
    const int wanted = mode == GAME_JOBS_DEFERRED || mode == GAME_JOBS_CHECK ? mode : GAME_JOBS_INLINE;
    if (wanted == GAME_JOBS_INLINE)
        GameJobs_Stop();
    else if (!s_onThread)
        s_onThread = StartThread();
    s_mode = s_onThread ? wanted : GAME_JOBS_INLINE;
    Sys_Log("game jobs: %s ([video] game_jobs = %d)",
            s_mode == GAME_JOBS_CHECK      ? "on the thread, and again inline to check"
            : s_mode == GAME_JOBS_DEFERRED ? "on the thread"
                                           : "inline",
            mode);
}

void GameJobs_Stop(void)
{
    if (s_onThread) {
        GameJobs_Wait(s_added.load());
        StopThread();
        s_onThread = false;
    }
    s_mode = GAME_JOBS_INLINE;
}

int GameJobs_Mode(void)
{
    if (s_mode < 0)
        GameJobs_Start(Sys_GetOption("video.game_jobs", GAME_JOBS_INLINE));
    return s_mode;
}

uint32_t GameJobs_Add(GameJobRun run, void *context)
{
    const uint32_t number = s_added.load() + 1;
    if (!s_onThread) {
        run(context);
        s_added.store(number);
        s_done.store(number);
        return number;
    }
    // A full ring: the job thread is behind by all of it.
    if (number - s_done.load() > kCapacity)
        GameJobs_Wait(number - kCapacity);
    s_ring[(number - 1) % kCapacity] = { run, context };
    s_added.store(number);
    Wake();
    return number;
}

void GameJobs_Wait(uint32_t number)
{
    LogFpscrOnce();
    if (Done(number))
        return;
    const unsigned long long start = Micros();
    Block(number);
    s_waitMicros.fetch_add((uint32_t)(Micros() - start));
}

uint32_t GameJobs_Hash(uint32_t hash, const void *data, size_t bytes)
{
    const unsigned char *p = (const unsigned char *)data;
    for (size_t i = 0; i < bytes; i++)
        hash = (hash ^ p[i]) * 16777619u;
    return hash;
}

void GameJobs_Check(const char *job, uint32_t inlineHash, uint32_t deferredHash)
{
    const uint32_t checks = s_checks.fetch_add(1) + 1;
    uint32_t mismatches = s_mismatches.load();
    if (inlineHash != deferredHash) {
        mismatches = s_mismatches.fetch_add(1) + 1;
        if (mismatches <= kLoggedMismatches)
            Sys_Log("game jobs: MISMATCH of %s at check %u: inline %08x, on the thread %08x", job, (unsigned)checks,
                    (unsigned)inlineHash, (unsigned)deferredHash);
    }
    if (checks % kChecksPerSummary == 0)
        Sys_Log("game jobs: %u checks, %u mismatches", (unsigned)checks, (unsigned)mismatches);
}

GameJobsTimes GameJobs_TakeTimes(void)
{
    GameJobsTimes times;
    times.jobs = s_jobMicros.exchange(0);
    times.waited = s_waitMicros.exchange(0);
    times.checks = s_checks.load();
    times.mismatches = s_mismatches.load();
    return times;
}
