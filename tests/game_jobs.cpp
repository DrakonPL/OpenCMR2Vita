// The game jobs (src/platform/game_jobs.cpp): inline, a job runs when it is
// added. On the thread a job runs later, in the order of the adds, also for
// more jobs than the ring holds, and a wait returns when the job is done. A
// pure job gives the same output on both. The setting gives the first mode.

#include "port/game_jobs.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string_view>
#include <thread>
#include <vector>

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "failed line %d: %s\n", __LINE__, #x); return 1; } } while (0)

namespace {

int s_settingMode = GAME_JOBS_CHECK;

} // namespace

extern "C" void Sys_Log(const char *, ...) {}
extern "C" int Sys_GetOption(const char *name, int fallback)
{
    return std::string_view(name) == "video.game_jobs" ? s_settingMode : fallback;
}

namespace {

// Waits for the gate for at most 2 s: a job run inside its add never sees it.
struct GatedJob {
    std::atomic<bool> gate{ false };
    std::atomic<bool> sawGate{ false };
    std::atomic<bool> done{ false };
};

void RunGated(void *context)
{
    GatedJob *job = (GatedJob *)context;
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!job->gate.load() && std::chrono::steady_clock::now() < end)
        std::this_thread::yield();
    job->sawGate = job->gate.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    job->done = true;
}

struct OrderJob {
    std::vector<int> *order;
    int value;
};

void RunOrder(void *context)
{
    const OrderJob *job = (const OrderJob *)context;
    job->order->push_back(job->value);
}

// A pure job like the tyre marks: 16.16 fixed point to floats.
struct ConvertJob {
    int input[512];
    float output[512];
};

void RunConvert(void *context)
{
    ConvertJob *job = (ConvertJob *)context;
    for (int i = 0; i < 512; i++)
        job->output[i] = (float)(job->input[i] * (1.0f / 65536.0f));
}

uint32_t HashOfConvert(int mode)
{
    GameJobs_Start(mode);
    ConvertJob job;
    for (int i = 0; i < 512; i++)
        job.input[i] = (int)((uint32_t)i * 2654435761u);
    GameJobs_Wait(GameJobs_Add(RunConvert, &job));
    return GameJobs_Hash(kGameJobsHashStart, job.output, sizeof(job.output));
}

int TestSettingGivesFirstMode()
{
    REQUIRE(GameJobs_Mode() == GAME_JOBS_CHECK);
    return 0;
}

int TestInlineRunsAtAdd()
{
    GameJobs_Start(GAME_JOBS_INLINE);
    REQUIRE(GameJobs_Mode() == GAME_JOBS_INLINE);
    GatedJob job;
    job.gate = true;
    GameJobs_Add(RunGated, &job);
    REQUIRE(job.done.load());
    return 0;
}

int TestThreadRunsLaterAndWaitWaits()
{
    GameJobs_Start(GAME_JOBS_DEFERRED);
    REQUIRE(GameJobs_Mode() == GAME_JOBS_DEFERRED);
    GameJobs_TakeTimes();
    GatedJob job;
    const uint32_t number = GameJobs_Add(RunGated, &job);
    job.gate = true;
    GameJobs_Wait(number);
    REQUIRE(job.sawGate.load());
    REQUIRE(job.done.load());
    const GameJobsTimes times = GameJobs_TakeTimes();
    REQUIRE(times.jobs >= 10000);
    REQUIRE(times.waited >= 10000);
    return 0;
}

int TestThreadKeepsOrder()
{
    GameJobs_Start(GAME_JOBS_DEFERRED);
    constexpr int kJobs = 100;
    std::vector<int> order;
    order.reserve(kJobs);
    OrderJob jobs[kJobs];
    uint32_t last = 0;
    for (int i = 0; i < kJobs; i++) {
        jobs[i] = { &order, i };
        last = GameJobs_Add(RunOrder, &jobs[i]);
    }
    GameJobs_Wait(last);
    REQUIRE((int)order.size() == kJobs);
    for (int i = 0; i < kJobs; i++)
        REQUIRE(order[i] == i);
    return 0;
}

int TestInlineEqualsDeferred()
{
    const uint32_t inlineHash = HashOfConvert(GAME_JOBS_INLINE);
    REQUIRE(inlineHash == HashOfConvert(GAME_JOBS_DEFERRED));
    return 0;
}

int TestCheckCounts()
{
    const GameJobsTimes before = GameJobs_TakeTimes();
    GameJobs_Check("same", 1, 1);
    GameJobs_Check("other", 1, 2);
    const GameJobsTimes after = GameJobs_TakeTimes();
    REQUIRE(after.checks == before.checks + 2);
    REQUIRE(after.mismatches == before.mismatches + 1);
    return 0;
}

int TestStopRunsInline()
{
    GameJobs_Start(GAME_JOBS_DEFERRED);
    GameJobs_Stop();
    REQUIRE(GameJobs_Mode() == GAME_JOBS_INLINE);
    GatedJob job;
    job.gate = true;
    GameJobs_Add(RunGated, &job);
    REQUIRE(job.done.load());
    return 0;
}

} // namespace

int main()
{
    if (TestSettingGivesFirstMode() || TestInlineRunsAtAdd() || TestThreadRunsLaterAndWaitWaits() ||
        TestThreadKeepsOrder() || TestInlineEqualsDeferred() || TestCheckCounts() || TestStopRunsInline())
        return 1;
    printf("game_jobs: ok\n");
    return 0;
}
