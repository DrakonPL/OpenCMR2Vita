// The batch jobs of the recorder (Gfx_RunBatchJob, gfx_internal.h) on a
// thread of their own. On the PS Vita the game's thread has core 0 and the
// executor's worker core 2: the batches of a frame of a wood took 8 to 10 ms
// of that worker, after the frame was recorded, and the game waited for them
// when it wrote vertices that they read. Core 1 (the sound) is idle for most
// of the time: the jobs run there while the game records the rest of the
// frame.
//
// The jobs run one at a time, in the order they were added. Without the
// thread a job runs when it is added. Each job has a number (1, 2, ...).
#ifndef OPENCMR2_GFX_BATCH_WORKER_H
#define OPENCMR2_GFX_BATCH_WORKER_H

#include "render/gfx_internal.h"

// Starts the thread. Returns whether the jobs run on it. Called by the
// thread that adds the jobs.
bool Gfx_StartBatchThread(void);
// Waits for the jobs, then stops the thread: jobs run when they are added.
void Gfx_StopBatchThread(void);

// Adds a job, and returns its number. Only one thread adds jobs. It waits
// here while the queue is full.
uint32_t Gfx_AddBatchJob(const GfxBatchJob &job);
// The number of the last job that was added (0: none), and of the last one
// that is done.
uint32_t Gfx_BatchJobsAdded(void);
uint32_t Gfx_BatchJobsDone(void);
// Returns when the jobs up to that number are done. waiter: 0 the thread
// that adds the jobs, 1 another one (the executor's worker), for the times.
void Gfx_WaitForBatchJobs(uint32_t number, int waiter);

// For the logs, in us since the last call: the time of the jobs, and the
// time each waiter waited.
struct GfxBatchTimes {
    unsigned jobs, waited[2];
};
GfxBatchTimes Gfx_TakeBatchTimes(void);

#endif
