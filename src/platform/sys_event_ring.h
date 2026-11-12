// A fixed queue of system events for a backend without an OS event queue
// (sys_vita.cpp). The oldest event is read first; a full queue drops new ones.
// One thread only.
#ifndef OPENCMR2_SYS_EVENT_RING_H
#define OPENCMR2_SYS_EVENT_RING_H

#include "port/sys.h"

class SysEventRing {
public:
    static constexpr int kCapacity = 64;

    bool Push(const SysEvent &event)
    {
        if (_count == kCapacity)
            return false;
        _events[(_head + _count) % kCapacity] = event;
        _count++;
        return true;
    }

    bool Pop(SysEvent *event)
    {
        if (_count == 0)
            return false;
        *event = _events[_head];
        _head = (_head + 1) % kCapacity;
        _count--;
        return true;
    }

    int Count() const { return _count; }

private:
    SysEvent _events[kCapacity] = {};
    int _head = 0;
    int _count = 0;
};

#endif
