// The tyre marks as a game job (CarEffects_DrawTyreMarks, game/StageObjects.cpp):
// for recorded trail points, the layer 0x20 that the job fills on the thread
// is the layer of the same marks made inline. The overflow of the layer goes
// to the layer's own flag, not to the flag of the other layers. A rewind of
// the layer and a second run give the same hash (the check of game_jobs = 2).

#include "port/types.h"
#include "port/game_jobs.h"
#include "EyePlane.h"
#include "FixedPoint.h"
#include "Sprite.h"

#include <cstdint>
#include <cstdio>

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "failed line %d: %s\n", __LINE__, #x); return 1; } } while (0)

// The layout of game/StageObjects.cpp.
struct TrailPoint {
    int age;
    int frame;
    FixVector left;
    FixVector right;
    BYTE colour[4];
    BYTE alphaLeft;
    BYTE alphaRight;
    BYTE material : 4;
    BYTE flags : 4;
    BYTE pad;
};
static_assert(sizeof(TrailPoint) == 0x28, "the layout of StageObjects.cpp");

extern TrailPoint g_trailPoints[8][4][200];
extern BYTE g_trailPointUsed[8][4][200];
extern int g_unk0x00549ba0[8][4];
extern BYTE g_quad2DConvertOverflow;
void CarEffects_DrawTyreMarks(int index, const EyePlane &eye);

namespace {

constexpr int kCars = 8;
constexpr int kStageSurfaces = 9;      // g_stageSurfaceInfo

uint32_t s_seed = 12345;

uint32_t Next()
{
    s_seed = s_seed * 1664525u + 1013904223u;
    return s_seed ^ (s_seed >> 16);
}

// Points laid in consecutive frames, some unused, on each surface, at
// positions within 1,000 m of the origin.
void RecordTrails()
{
    for (int car = 0; car < kCars; car++)
        for (int wheel = 0; wheel < 4; wheel++) {
            g_unk0x00549ba0[car][wheel] = (int)(Next() % 200);
            for (int i = 0; i < 200; i++) {
                TrailPoint &p = g_trailPoints[car][wheel][i];
                g_trailPointUsed[car][wheel][i] = Next() % 8 != 0;
                p.age = (int)(Next() % 0x600);
                p.frame = i;
                p.left = { (int)(Next() % 0x7d00000) - 0x3e80000, (int)(Next() % 0x100000),
                           (int)(Next() % 0x7d00000) - 0x3e80000 };
                p.right = { p.left.x + 0x10000, p.left.y, p.left.z + (int)(Next() % 0x8000) };
                for (BYTE &c : p.colour)
                    c = (BYTE)(Next() % 256);
                p.alphaLeft = (BYTE)Next();
                p.alphaRight = (BYTE)Next();
                p.material = Next() % kStageSurfaces;
            }
        }
}

// The eye at the origin, looking along +z: half the points are behind it.
const EyePlane kEye = { 0, 0, 1 << 28, 0 };

void DrawAllCars(void *)
{
    for (int car = 0; car < kCars; car++)
        CarEffects_DrawTyreMarks(car, kEye);
}

int TestThreadEqualsInline()
{
    RecordTrails();
    const Quad2DLayerCMark start = Quad2D_MarkLayerC();

    GameJobs_Start(GAME_JOBS_INLINE);
    GameJobs_Add(DrawAllCars, nullptr);
    const Quad2DLayerCMark inlineEnd = Quad2D_MarkLayerC();
    const unsigned int inlineHash = Quad2D_HashLayerCSince(start);
    // Marks queued, marks behind the eye, and more than the layer holds.
    REQUIRE(inlineEnd.count > start.count + 100);
    REQUIRE(inlineEnd.leftOut > start.leftOut + 100);
    REQUIRE(inlineEnd.overflow == 1);
    REQUIRE(g_quad2DConvertOverflow == 0);

    Quad2D_RewindLayerC(start);
    REQUIRE(Quad2D_HashLayerCSince(start) != inlineHash);
    GameJobs_Start(GAME_JOBS_DEFERRED);
    GameJobs_Wait(GameJobs_Add(DrawAllCars, nullptr));
    REQUIRE(Quad2D_HashLayerCSince(start) == inlineHash);
    REQUIRE(g_quad2DConvertOverflow == 0);
    GameJobs_Stop();
    return 0;
}

int TestRewindGivesSameHash()
{
    RecordTrails();
    Quad2D_RewindLayerC({ 0, 0, 0 });
    const Quad2DLayerCMark start = Quad2D_MarkLayerC();
    DrawAllCars(nullptr);
    const unsigned int first = Quad2D_HashLayerCSince(start);
    Quad2D_RewindLayerC(start);
    DrawAllCars(nullptr);
    REQUIRE(Quad2D_HashLayerCSince(start) == first);
    // Another input gives another hash.
    g_trailPoints[0][0][0].left.x ^= 0x10000;
    g_trailPoints[1][1][1].alphaLeft ^= 0x40;
    Quad2D_RewindLayerC(start);
    DrawAllCars(nullptr);
    REQUIRE(Quad2D_HashLayerCSince(start) != first);
    return 0;
}

} // namespace

int main()
{
    if (TestThreadEqualsInline() || TestRewindGivesSameHash())
        return 1;
    printf("tyre_mark_job: ok\n");
    return 0;
}
