// The thread, the lock and the seed of a net::Session. In the PS Vita build
// pthread starts no thread, and a function-local static or a thread_local goes
// through pthread too: the kernel's objects serve there. The PC has the
// standard ones.
#ifndef OPENCMR2_NET_OS_H
#define OPENCMR2_NET_OS_H

#include <stdint.h>

#include <chrono>
#include <mutex>

#ifdef VITA
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/rng.h>
#include <psp2/kernel/threadmgr.h>

// One session at a time on the Vita (net_api.cpp): a plain global serves.
#define NET_THREAD_LOCAL

class NetMutex {
public:
    NetMutex() : _id(sceKernelCreateMutex("net_session", 0, 0, NULL)) {}
    ~NetMutex()
    {
        if (_id >= 0)
            sceKernelDeleteMutex(_id);
    }
    NetMutex(const NetMutex &) = delete;
    NetMutex &operator=(const NetMutex &) = delete;
    void lock() { sceKernelLockMutex(_id, 1, NULL); }
    void unlock() { sceKernelUnlockMutex(_id, 1); }

private:
    SceUID _id;
};

// Only Open(JOIN) waits, a few seconds at most: a poll every 5 ms serves.
class NetCondition {
public:
    template <class Done>
    bool wait_for(std::unique_lock<NetMutex> &lock, std::chrono::milliseconds timeout, Done done)
    {
        const SceUInt64 end = sceKernelGetProcessTimeWide() + (SceUInt64)timeout.count() * 1000;
        while (!done()) {
            if (sceKernelGetProcessTimeWide() >= end)
                return false;
            lock.unlock();
            sceKernelDelayThread(5000);
            lock.lock();
        }
        return true;
    }
    void notify_all() {}
};

class NetThread {
public:
    NetThread() = default;
    NetThread(void (*entry)(void *), void *argument)
    {
        // The service thread holds message buffers: more than the 64 KB default.
        constexpr SceSize kStackBytes = 128 * 1024;
        constexpr int kPriority = 150;     // above the game (160), below the batch thread (140)
        _id = sceKernelCreateThread("net_session", Entry, kPriority, kStackBytes, 0, SCE_KERNEL_CPU_MASK_USER_1, NULL);
        Call call = { entry, argument };
        if (_id >= 0 && sceKernelStartThread(_id, sizeof(call), &call) < 0) {
            sceKernelDeleteThread(_id);
            _id = -1;
        }
    }
    NetThread(NetThread &&other) noexcept : _id(other._id) { other._id = -1; }
    NetThread &operator=(NetThread &&other) noexcept
    {
        SceUID id = _id;
        _id = other._id;
        other._id = id;
        return *this;
    }
    NetThread(const NetThread &) = delete;
    NetThread &operator=(const NetThread &) = delete;
    bool joinable() const { return _id >= 0; }
    void join()
    {
        sceKernelWaitThreadEnd(_id, NULL, NULL);
        sceKernelDeleteThread(_id);
        _id = -1;
    }

private:
    struct Call {
        void (*entry)(void *);
        void *argument;
    };
    // The kernel copies the Call onto the new thread's stack.
    static int Entry(SceSize, void *data)
    {
        const Call *call = (const Call *)data;
        call->entry(call->argument);
        return 0;
    }
    SceUID _id = -1;
};

inline uint64_t NetRandomSeed()
{
    uint64_t seed = 0;
    if (sceKernelGetRandomNumber(&seed, sizeof(seed)) < 0)
        seed = sceKernelGetProcessTimeWide();
    return seed;
}

#else
#include <condition_variable>
#include <random>
#include <thread>

#define NET_THREAD_LOCAL thread_local

using NetMutex = std::mutex;
using NetCondition = std::condition_variable;
using NetThread = std::thread;

inline uint64_t NetRandomSeed()
{
    std::random_device device;
    return ((uint64_t)device() << 32) ^ device();
}
#endif

#endif
