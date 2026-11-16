// PORT: the merge of the ground parts of a frame into one draw for each
// texture (Game_DrawMergedGroundParts), kept from frame to frame. The game
// put each part in view into its group, with a search of the groups for
// each part, and wrote the indices of each group again, in each frame. The
// parts in view are the same as in the frame before in most frames (85% in
// the stage probe): the groups and their indices are then those of the
// frame before. A new list of parts makes them again, with a table for the
// group of a part.
// A part that is nearer than the start of the distance fade when the groups
// are made goes into a group of such parts (FadeReach.h): the renderer draws
// that group without the fade. Each draw comes with the reach of its parts
// as they are now, so a group that stays while the view moves is right: the
// renderer draws it with the fade when one of its parts reaches the start.
// No game state here: tests/ground_merge.cpp checks it on the PC against a
// merge that is made again each time.
#ifndef OPENCMR2_GROUND_MERGE_H
#define OPENCMR2_GROUND_MERGE_H

#include <string.h>

// What the merge holds. A frame with more draws its ground part by part.
enum { kGroundMergeGroups = 1024, kGroundMergeParts = 4096, kGroundMergeIndices = 1 << 18 };

// A part of a ground mesh in view. indices: relative to first.
struct GroundMergePart {
    const void *mesh, *part;
    const unsigned short *indices;
    int indexCount;
    int first, end;             // its vertices in the vertex buffer
    int buffer, texture, cull, lighting;
    // Whether each of its vertices is nearer than the start of the distance
    // fade, and how far from the eye its farthest one can be (below zero:
    // not known). They change with the view: a part with others is the same
    // part.
    int beforeFade;
    float reach;
};

// The parts of one vertex buffer, texture, cull mode, lighting and side of
// the start of the fade: one draw. Its indices are relative to first.
// reach: the farthest of its parts as they are now (below zero: not known).
struct GroundMergeGroup {
    int buffer, texture, cull, lighting;
    int first, end;
    unsigned indexStart, indexCount;
    int beforeFade;
    float reach;
};

struct GroundMerge {
    GroundMergeGroup groups[kGroundMergeGroups];
    unsigned groupCount;
    unsigned short indices[kGroundMergeIndices];
    // The parts that the groups were made of (none: no groups).
    GroundMergePart parts[kGroundMergeParts];
    unsigned partCount;
    // For the logs and the tests: how often the groups were made, and kept.
    unsigned made, kept;
    // One more each time the groups and the indices are made: a copy of the
    // indices (an index buffer) with another number is old.
    unsigned generation;
    // While the groups are made: the group of each part, and the groups by
    // a hash of what makes a group (the index of the group + 1, 0: free).
    unsigned short partGroup[kGroundMergeParts];
    unsigned short table[2 * kGroundMergeGroups];
};

inline bool GroundMerge_SamePart(const GroundMergePart &a, const GroundMergePart &b)
{
    return a.mesh == b.mesh && a.part == b.part && a.indices == b.indices && a.indexCount == b.indexCount &&
           a.first == b.first && a.end == b.end && a.buffer == b.buffer && a.texture == b.texture &&
           a.cull == b.cull && a.lighting == b.lighting;
}

// The merge is of no stage: the next update makes the groups (the meshes
// of a stage that goes can come back at the same addresses).
inline void GroundMerge_Forget(GroundMerge *m)
{
    m->partCount = 0;
    m->groupCount = 0;
}

// The reach of each group from those of its parts (the parts of the groups,
// as the view has them now).
inline void GroundMerge_SetReaches(GroundMerge *m, const GroundMergePart *parts, unsigned count)
{
    for (unsigned g = 0; g < m->groupCount; g++)
        m->groups[g].reach = 0.0f;
    for (unsigned i = 0; i < count; i++) {
        GroundMergeGroup &group = m->groups[m->partGroup[i]];
        if (group.reach >= 0.0f)
            group.reach = parts[i].reach < 0.0f ? -1.0f : parts[i].reach > group.reach ? parts[i].reach : group.reach;
    }
}

// The groups and the indices for the parts in view, in their order: those
// that are there, if the parts are the same, else made again. The groups
// are in the order of their first parts, and the indices of a group in the
// order of its parts. False: more groups or indices than the merge holds
// (the caller then draws the ground part by part).
inline bool GroundMerge_Update(GroundMerge *m, const GroundMergePart *parts, unsigned count)
{
    unsigned i;

    if (count > kGroundMergeParts) {
        GroundMerge_Forget(m);
        return false;
    }
    if (count == m->partCount) {
        for (i = 0; i < count && GroundMerge_SamePart(parts[i], m->parts[i]); i++)
            ;
        if (i == count) {
            m->kept++;
            GroundMerge_SetReaches(m, parts, count);
            return true;
        }
    }
    GroundMerge_Forget(m);
    memset(m->table, 0, sizeof(m->table));
    const unsigned mask = 2 * kGroundMergeGroups - 1;
    unsigned indexCount = 0;
    for (i = 0; i < count; i++) {
        const GroundMergePart &p = parts[i];
        // The two groups of a texture (the sides of the fade) start at one
        // place of the table: the second is one step on.
        unsigned h = (unsigned)p.buffer * 2654435761u ^ (unsigned)p.texture * 2246822519u ^
                     (unsigned)p.cull * 3266489917u ^ (unsigned)p.lighting * 668265263u;
        h = (h ^ h >> 15) & mask;
        unsigned g;
        for (;; h = (h + 1) & mask) {
            if (m->table[h] == 0) {
                if (m->groupCount == kGroundMergeGroups) {
                    m->groupCount = 0;
                    return false;
                }
                g = m->groupCount++;
                m->table[h] = (unsigned short)(g + 1);
                const GroundMergeGroup group = { p.buffer, p.texture, p.cull, p.lighting, p.first, p.end, 0, 0,
                                                 p.beforeFade, 0.0f };
                m->groups[g] = group;
                break;
            }
            g = m->table[h] - 1u;
            const GroundMergeGroup &group = m->groups[g];
            if (group.buffer == p.buffer && group.texture == p.texture && group.cull == p.cull &&
                group.lighting == p.lighting && group.beforeFade == p.beforeFade)
                break;
        }
        GroundMergeGroup &group = m->groups[g];
        group.first = p.first < group.first ? p.first : group.first;
        group.end = p.end > group.end ? p.end : group.end;
        const unsigned n = p.indexCount > 0 ? (unsigned)p.indexCount : 0;
        group.indexCount += n;
        indexCount += n;
        if (indexCount > kGroundMergeIndices) {
            m->groupCount = 0;
            return false;
        }
        m->partGroup[i] = (unsigned short)g;
    }
    unsigned start = 0;
    for (unsigned g = 0; g < m->groupCount; g++) {
        m->groups[g].indexStart = start;
        start += m->groups[g].indexCount;
        m->groups[g].indexCount = 0;
    }
    for (i = 0; i < count; i++) {
        const GroundMergePart &p = parts[i];
        GroundMergeGroup &group = m->groups[m->partGroup[i]];
        const int base = p.first - group.first;
        unsigned short *out = &m->indices[group.indexStart + group.indexCount];
        for (int k = 0; k < p.indexCount; k++)
            out[k] = (unsigned short)(p.indices[k] + base);
        group.indexCount += p.indexCount > 0 ? (unsigned)p.indexCount : 0;
        m->parts[i] = p;
    }
    m->partCount = count;
    m->made++;
    m->generation++;
    GroundMerge_SetReaches(m, parts, count);
    return true;
}

// The indices of all groups (the groups are in the order of their indices).
inline unsigned GroundMerge_IndexCount(const GroundMerge *m)
{
    if (m->groupCount == 0)
        return 0;
    const GroundMergeGroup &last = m->groups[m->groupCount - 1];
    return last.indexStart + last.indexCount;
}

#endif
