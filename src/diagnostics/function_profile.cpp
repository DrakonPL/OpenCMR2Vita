// The time of each function of the game, for a profiling build
// (OPENCMR2_PROFILE_FUNCTIONS): the compiler calls the two hooks below at the
// entry and the exit of each function in game/*.cpp. A frame is one call of
// CGame::DispatchFrontendResourceState. The hooks time the functions down to
// kMaxDepth calls below it, and the log gets the functions with the most time
// each kReportFrames frames, by their addresses (tools: addr2line, with the
// address of the frame function that each report gives).
//
// The hooks cost time themselves: a profiling build is slower than a normal
// build, and a function that calls many small functions looks slower than it
// is. A function that is called more than kTimedCalls times in a frame is
// timed for its first calls only, and its time is scaled to all its calls.
//
// The game runs on one thread: the hooks keep no lock.

#include "port/sys.h"

#include <stddef.h>
#include <stdint.h>

#include <algorithm>

#ifdef VITA
#include <psp2/kernel/processmgr.h>
static inline uint64_t Now()
{
    return sceKernelGetProcessTimeWide();
}
#else
#include <chrono>
static inline uint64_t Now()
{
    return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
#endif

// CGame::DispatchFrontendResourceState(), by its linker name: this file does
// not see the game's headers.
extern "C" void _ZN5CGame29DispatchFrontendResourceStateEv();

namespace {

constexpr int kMaxDepth = 12;
constexpr uint32_t kTimedCalls = 32;
constexpr unsigned kTableSize = 8192;       // a power of two
constexpr uint32_t kReportFrames = 300;
constexpr unsigned kReportLines = 60;

struct Entry {
    void *function;
    uint64_t micros;        // of the timed calls
    uint32_t calls, timed;  // in the frames of the report
    uint32_t frame, frameCalls;
    int depth;              // the least depth it was called at
};
Entry s_table[kTableSize];

struct Open {
    Entry *entry;           // NULL: not timed
    uint64_t start;
};
Open s_stack[kMaxDepth];
int s_depth = -1;           // below the frame function; -1: not in a frame
uint32_t s_frame, s_frames;
uint64_t s_frameStart, s_frameMicros;

Entry *Find(void *function)
{
    unsigned i = (unsigned)(((uintptr_t)function >> 1) * 2654435761u) >> 19 & (kTableSize - 1);
    for (unsigned probes = 0; probes < 64; probes++) {
        Entry &e = s_table[i];
        if (e.function == function)
            return &e;
        if (e.function == nullptr) {
            e.function = function;
            e.depth = kMaxDepth;
            return &e;
        }
        i = (i + 1) & (kTableSize - 1);
    }
    return nullptr;
}

void Report()
{
    static Entry *order[kTableSize];
    unsigned count = 0;
    for (Entry &e : s_table)
        if (e.function != nullptr && e.timed > 0)
            order[count++] = &e;
    auto time = [](const Entry *e) { return (double)e->micros * e->calls / e->timed; };
    std::sort(order, order + count, [&](const Entry *a, const Entry *b) { return time(a) > time(b); });
    Sys_Log("profile: %u frames of %.2f ms, the frame function at %p, %u functions", s_frames,
            s_frameMicros / 1000.0 / s_frames, (void *)&_ZN5CGame29DispatchFrontendResourceStateEv, count);
    for (unsigned i = 0; i < count && i < kReportLines; i++) {
        const Entry *e = order[i];
        Sys_Log("profile: %p depth %d: %.3f ms, %.1f calls for each frame", e->function, e->depth,
                time(e) / 1000.0 / s_frames, (double)e->calls / s_frames);
    }
    for (Entry &e : s_table) {
        e.micros = 0;
        e.calls = e.timed = 0;
    }
    s_frames = 0;
    s_frameMicros = 0;
}

} // namespace

extern "C" void __cyg_profile_func_enter(void *function, void *)
{
    if (s_depth < 0) {
        if (function != (void *)&_ZN5CGame29DispatchFrontendResourceStateEv)
            return;
        s_depth = 0;
        s_frame++;
        s_frameStart = Now();
        return;
    }
    if (s_depth < kMaxDepth) {
        Open &open = s_stack[s_depth];
        open.entry = nullptr;
        if (Entry *e = Find(function)) {
            if (e->frame != s_frame) {
                e->frame = s_frame;
                e->frameCalls = 0;
            }
            e->calls++;
            e->depth = s_depth < e->depth ? s_depth : e->depth;
            if (e->frameCalls++ < kTimedCalls) {
                open.entry = e;
                open.start = Now();
            }
        }
    }
    s_depth++;
}

extern "C" void __cyg_profile_func_exit(void *function, void *)
{
    if (s_depth < 0)
        return;
    if (s_depth == 0) {
        // The frame function returns.
        s_frameMicros += Now() - s_frameStart;
        s_depth = -1;
        if (++s_frames >= kReportFrames)
            Report();
        return;
    }
    s_depth--;
    if (s_depth < kMaxDepth) {
        const Open &open = s_stack[s_depth];
        if (open.entry != nullptr) {
            open.entry->micros += Now() - open.start;
            open.entry->timed++;
        }
    }
}
