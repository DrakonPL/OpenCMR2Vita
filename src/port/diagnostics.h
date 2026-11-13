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
}
#endif
