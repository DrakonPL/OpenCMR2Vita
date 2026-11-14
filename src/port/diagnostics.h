/* Optional observation of the existing update loop; no scheduler replacement. */
#ifndef OPENCMR2_PORT_DIAGNOSTICS_H
#define OPENCMR2_PORT_DIAGNOSTICS_H

namespace Diagnostics {
// The parts of the scene that the game draws for a view (CGame's scene
// function names the part that starts, for Observer::ScenePart).
enum {
    SCENE_PART_NONE,        // not in the scene function
    SCENE_PART_SETUP,       // the background layer, the sector culling and the relighting
    SCENE_PART_PARTICLES,
    SCENE_PART_SKY,         // the nodes without depth
    SCENE_PART_GROUND,      // the meshes of the sectors
    SCENE_PART_SHADOWS,     // the shadow meshes of the sectors
    SCENE_PART_OBJECTS,     // the static stage objects
    SCENE_PART_BILLBOARDS,  // the stage objects that are drawn sorted
    SCENE_PART_NODES,       // the nodes of the sectors: the cars and what moves
    SCENE_PART_LAYERS,      // the glow, the 2D and 3D layers, the lines
    // The parts of the update of a view, before its scene is drawn.
    SCENE_PART_VIEW,        // the rest of the update of the view
    SCENE_PART_PREPARE,     // Game_PrepareScene
    SCENE_PART_CAR_SHADOWS, // the shadow geometry of the cars
    SCENE_PART_REFLECTIONS, // the cube maps of the car reflections, and the view masks of the cars
    SCENE_PART_HUD,         // the dash and the HUD of the view
    // The parts inside the parts above: their time is not in the part around them.
    SCENE_PART_OFF_ROAD,    // the off-road state of the cars in the view (view)
    SCENE_PART_TYRE_MARKS,  // the tyre mark triangles (view)
    SCENE_PART_SECTOR_CULL, // the culling of the sector grid (setup)
    SCENE_PART_RELIGHT,     // the relighting of the visible sectors (setup)
    SCENE_PART_DEFERRED_SORT, // the order of the sorted stage objects (billboards)
    SCENE_PART_GLOW,        // Glow_Draw (layers)
    SCENE_PART_QUADS,       // Quad2D_DrawLayer: the 3D quads and triangles of a layer (layers)
    SCENE_PART_2D,          // the 2D sprites, triangles and lines of a layer (setup, layers)
    SCENE_PART_CAR_NODES,   // the nodes, the matrices and the weather lighting of the view (view)
    SCENE_PART_WHEELS,      // the skid effects and the ground lighting of the wheels (view)
    SCENE_PART_SKID_TRAILS, // the exhaust points and the new skid trail points (view)
    SCENE_PART_VERTEX_WRITES, // the meshes written into their vertex buffers (Mesh_RefreshVertices; relight)
    SCENE_PART_COUNT
};

// The names of the parts, for the logs.
inline constexpr const char *kScenePartNames[] = {
    "other", "setup", "particles", "sky", "ground", "shadows", "objects", "billboards",
    "nodes", "layers", "view", "prepare", "car shadows", "reflections", "hud", "off road", "tyre marks",
    "sector cull", "relight", "deferred sort", "glow", "quads", "2d", "car nodes", "wheels", "skid trails",
    "vertex writes"
};
static_assert(sizeof(kScenePartNames) / sizeof(kScenePartNames[0]) == SCENE_PART_COUNT, "a name for each scene part");

struct Observer {
    virtual ~Observer() = default;
    virtual void BeginTick(unsigned char state) {}
    virtual void ControlsReady() {}
    virtual void EndTick() {}
    virtual void BeginFrame() {}
    virtual void EndFrame() {}
    virtual void BeginPresent() {}
    virtual void EndPresent() {}
    // A part of the scene starts (SCENE_PART_*): the part before it ends.
    virtual void ScenePart(int part) {}
    // The part that runs now.
    virtual int Part() const { return SCENE_PART_NONE; }
};
extern Observer *observer;

// Names the part that starts. No observer: no cost.
inline void ScenePart(int part)
{
    if (observer)
        observer->ScenePart(part);
}

// A part for the rest of a function: no part when the function returns.
struct ScenePartScope {
    explicit ScenePartScope(int part) { ScenePart(part); }
    ~ScenePartScope() { ScenePart(SCENE_PART_NONE); }
};

// A part inside another for the rest of a block: the outer part goes on
// after it. No observer: no cost.
struct ScenePartNest {
    explicit ScenePartNest(int part) : outer(observer ? observer->Part() : -1)
    {
        if (outer >= 0)
            observer->ScenePart(part);
    }
    ~ScenePartNest()
    {
        if (outer >= 0 && observer)
            observer->ScenePart(outer);
    }
    ScenePartNest(const ScenePartNest &) = delete;
    ScenePartNest &operator=(const ScenePartNest &) = delete;

private:
    const int outer;    // -1: no observer at the start
};
}
#endif
