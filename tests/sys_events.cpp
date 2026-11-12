// The Vita backend's event queue (src/platform/sys_event_ring.h): fixed size,
// oldest first, new events dropped when full.

#include "platform/sys_event_ring.h"

#include <cstdio>

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "failed line %d: %s\n", __LINE__, #x); return 1; } } while (0)

int main()
{
    SysEventRing ring;
    SysEvent event = {};
    REQUIRE(!ring.Pop(&event));

    int accepted = 0;
    for (int i = 0; i < 100; i++) {
        SysEvent pushed = {};
        pushed.type = SYS_EVENT_KEY_DOWN;
        pushed.key = i;
        accepted += ring.Push(pushed) ? 1 : 0;
    }
    REQUIRE(accepted == SysEventRing::kCapacity && ring.Count() == SysEventRing::kCapacity);

    int read = 0;
    while (ring.Pop(&event)) {
        REQUIRE(event.type == SYS_EVENT_KEY_DOWN && event.key == read);
        read++;
    }
    REQUIRE(read == 64);

    // The read and write positions wrap around the end of the array.
    for (int round = 0; round < 3 * SysEventRing::kCapacity; round++) {
        SysEvent pushed = {};
        pushed.type = SYS_EVENT_FOCUS_LOST + round % 2;
        pushed.key = round;
        REQUIRE(ring.Push(pushed));
        REQUIRE(ring.Pop(&event) && event.key == round && event.type == pushed.type);
    }
    REQUIRE(ring.Count() == 0);
    puts("sys_events: ok");
    return 0;
}
