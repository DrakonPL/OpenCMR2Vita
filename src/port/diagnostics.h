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
    SCENE_PART_VIEW,        // the nodes of the view, the weather lighting, the skid effects
    SCENE_PART_PREPARE,     // Game_PrepareScene
    SCENE_PART_CAR_SHADOWS, // the shadow geometry of the cars
    SCENE_PART_REFLECTIONS, // the cube maps of the car reflections, and the view masks of the cars
    SCENE_PART_HUD,         // the dash and the HUD of the view
    SCENE_PART_COUNT
};

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
}
#endif
