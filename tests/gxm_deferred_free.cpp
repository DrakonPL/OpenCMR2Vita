// The free list of the GXM textures (render/gxm_deferred_free.h): a block
// goes back kGxmFrameSlots frames after its texture was destroyed, never while
// a frame that may draw with it is still on the GPU.

#include "render/gxm_deferred_free.h"

#include <cstdio>
#include <vector>

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "failed line %d: %s\n", __LINE__, #x); return 1; } } while (0)

int main()
{
    REQUIRE(kGxmFrameSlots == 3);
    int a = 0, b = 0, c = 0;
    GxmDeferredFree list;
    std::vector<void *> released;
    auto release = [&released](void *memory) { released.push_back(memory); };

    // Destroyed while frame 10 is recorded: frames 10, 11 and 12 may still be
    // drawn with it, so it goes back at the start of frame 13.
    list.Add(&a, 10);
    list.Collect(10, release);
    list.Collect(11, release);
    list.Add(&b, 11);
    list.Collect(12, release);
    REQUIRE(released.empty());
    list.Collect(13, release);
    REQUIRE(released.size() == 1 && released[0] == &a);
    REQUIRE(list.entries.size() == 1);
    list.Collect(14, release);
    REQUIRE(released.size() == 2 && released[1] == &b && list.entries.empty());

    // Across the wrap of the frame counter.
    released.clear();
    list.Add(&c, 0xffffffffu);
    list.Collect(1, release);
    REQUIRE(released.empty());
    list.Collect(2, release);
    REQUIRE(released.size() == 1 && released[0] == &c);

    printf("gxm_deferred_free: ok\n");
    return 0;
}
