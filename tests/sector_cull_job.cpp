// The sector cull as a game job (Game_StartSectorCull, game/Game.cpp): the
// job on the thread marks the sectors that the cull on the game's thread
// marks, for the same camera. The job reads its own copy of the camera, so
// a camera that moves after the start does not change the job's sectors.

#include "port/types.h"
#include "port/game_jobs.h"
#include "Sector.h"
#include "SceneNode.h"
#include "Graphics.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "failed line %d: %s\n", __LINE__, #x); return 1; } } while (0)

extern void Scene_InitFixedMathTables();
extern void Sector_CullGridAroundViewNode(SceneNode *, void *);
void Game_StartSectorCull(SceneNode *pCamera);
void Game_JoinSectorCull(void);

namespace {

constexpr int kRows = 16, kColumns = 16, kCount = kRows * kColumns, kSize = 20;

struct Visible {
    int bits[128];
    std::vector<unsigned short> indices;
};

Visible Take()
{
    Visible v;
    memcpy(v.bits, g_sectorVisibleBits, sizeof(v.bits));
    v.indices.assign(g_visibleSectorIndices, g_visibleSectorIndices + g_sectorCullEnabled);
    return v;
}

bool Same(const Visible &a, const Visible &b)
{
    return memcmp(a.bits, b.bits, sizeof(a.bits)) == 0 && a.indices == b.indices;
}

} // namespace

int main()
{
    Scene_InitFixedMathTables();
    static Sector cells[kCount];
    g_sectorsPerRow = kColumns;
    g_sectorRows = kRows;
    g_sectorCount = kCount;
    g_sectorSize = kSize * 65536;
    g_sectorHalfSize = kSize * 32768;
    g_sectorScale = 65536 / kSize;
    for (int i = 0; i < kCount; i++) {
        Sector &s = cells[i];
        s.x = (i % kColumns) * kSize * 65536;
        s.z = -(i / kColumns) * kSize * 65536;
        const int loX = s.x - g_sectorHalfSize, hiX = s.x + g_sectorHalfSize;
        const int loZ = s.z - g_sectorHalfSize, hiZ = s.z + g_sectorHalfSize;
        const int rect[4][2] = { { loX, loZ }, { hiX, loZ }, { hiX, hiZ }, { loX, hiZ } };
        memcpy(s.bounds, rect, sizeof(rect));
        g_sectors[i] = &s;
    }
    CGraphics::m_unk0x0072d56c = 1;
    CGraphics::m_farPlaneFixed = 120 * 65536;
    CGraphics::m_projection11 = 0.75f;

    int compared = 0;
    for (int yaw = 0; yaw < 16; yaw++) {
        SceneNode camera = {};
        const double angle = yaw * 3.141592653589793 / 8;
        camera.world.position = { 8 * kSize * 65536, 0, -8 * kSize * 65536 };
        camera.world.forward = { (int)llround(sin(angle) * 65536), 0, (int)llround(cos(angle) * 65536) };

        GameJobs_Start(GAME_JOBS_INLINE);
        Sector_CullGridAroundViewNode(&camera, NULL);
        const Visible expected = Take();
        REQUIRE(expected.indices.size() > 10);

        memset(g_sectorVisibleBits, 0, sizeof(g_sectorVisibleBits));
        g_sectorCullEnabled = 0;
        GameJobs_Start(GAME_JOBS_DEFERRED);
        Game_StartSectorCull(&camera);
        // The camera turns after the start: the job keeps the first view.
        camera.world.forward = { -camera.world.forward.x, 0, -camera.world.forward.z };
        Game_JoinSectorCull();
        REQUIRE(Same(Take(), expected));
        compared++;
    }
    GameJobs_Stop();
    REQUIRE(compared == 16);
    printf("sector_cull_job: ok\n");
    return 0;
}
