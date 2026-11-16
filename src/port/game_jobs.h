/* Pure render work of the game as jobs on a thread of their own: core 1 on
   the PS Vita, a plain thread on the PC. A job reads only data that no code
   changes before its join, and writes only its own outputs. The jobs run one
   at a time, in the order they were added. Each job has a number (1, 2, ...).

   [video] game_jobs: 0 runs each job when it is added, 1 runs it on the
   thread, 2 also runs it on the thread, and the caller runs it again at the
   join and compares the two outputs (GameJobs_Check). */
#ifndef OPENCMR2_GAME_JOBS_H
#define OPENCMR2_GAME_JOBS_H

#include <stddef.h>
#include <stdint.h>

enum GameJobsMode {
    GAME_JOBS_INLINE = 0,
    GAME_JOBS_DEFERRED = 1,
    GAME_JOBS_CHECK = 2,
};

typedef void (*GameJobRun)(void *context);

/* Sets the mode and starts or stops the thread. A thread that does not start
   gives GAME_JOBS_INLINE. Called by the thread that adds the jobs. */
void GameJobs_Start(int mode);
/* Waits for the jobs, then stops the thread: GAME_JOBS_INLINE. */
void GameJobs_Stop(void);
/* The mode. The first call without a GameJobs_Start reads [video] game_jobs. */
int GameJobs_Mode(void);

/* Adds a job, and returns its number. Without the thread the job runs here. */
uint32_t GameJobs_Add(GameJobRun run, void *context);
/* Returns when the jobs up to that number are done. */
void GameJobs_Wait(uint32_t number);

/* FNV-1a of the bytes, from the hash of the bytes before them (start with
   kGameJobsHashStart). */
static const uint32_t kGameJobsHashStart = 2166136261u;
uint32_t GameJobs_Hash(uint32_t hash, const void *data, size_t bytes);
/* Counts a compare of GAME_JOBS_CHECK, and logs a mismatch. */
void GameJobs_Check(const char *job, uint32_t inlineHash, uint32_t deferredHash);

/* For the logs: since the last call, the time of the jobs on the thread and
   the time the adding thread waited for them (us). Since the start, the
   compares of GAME_JOBS_CHECK and their mismatches. */
struct GameJobsTimes {
    unsigned jobs, waited;
    unsigned checks, mismatches;
};
GameJobsTimes GameJobs_TakeTimes(void);

#endif
