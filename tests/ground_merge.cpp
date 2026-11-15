// The merge of the ground parts of a frame, kept from frame to frame
// (game/GroundMerge.h): the same groups and indices as a merge that is made
// again each time, kept while the parts are the same, made again when a
// part differs in anything.

#include "GroundMerge.h"

#include <cstdio>
#include <vector>

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "failed line %d: %s\n", __LINE__, #x); return 1; } } while (0)

namespace {

unsigned s_seed = 90210;

unsigned Next()
{
    s_seed = s_seed * 1664525u + 1013904223u;
    return s_seed >> 8;
}

// The merge as the game made it in each frame: a search of the groups for
// each part.
struct Reference {
    std::vector<GroundMergeGroup> groups;
    std::vector<unsigned short> indices;
};

Reference MergeAgain(const std::vector<GroundMergePart> &parts)
{
    Reference r;
    std::vector<unsigned> partGroup;
    for (const GroundMergePart &p : parts) {
        unsigned g;
        for (g = 0; g < r.groups.size(); g++) {
            const GroundMergeGroup &group = r.groups[g];
            if (group.buffer == p.buffer && group.texture == p.texture && group.cull == p.cull &&
                group.lighting == p.lighting)
                break;
        }
        if (g == r.groups.size()) {
            const GroundMergeGroup group = { p.buffer, p.texture, p.cull, p.lighting, p.first, p.end, 0, 0 };
            r.groups.push_back(group);
        }
        GroundMergeGroup &group = r.groups[g];
        group.first = p.first < group.first ? p.first : group.first;
        group.end = p.end > group.end ? p.end : group.end;
        group.indexCount += (unsigned)p.indexCount;
        partGroup.push_back(g);
    }
    unsigned start = 0;
    for (GroundMergeGroup &group : r.groups) {
        group.indexStart = start;
        start += group.indexCount;
        group.indexCount = 0;
    }
    r.indices.resize(start);
    for (size_t i = 0; i < parts.size(); i++) {
        const GroundMergePart &p = parts[i];
        GroundMergeGroup &group = r.groups[partGroup[i]];
        for (int k = 0; k < p.indexCount; k++)
            r.indices[group.indexStart + group.indexCount + k] = (unsigned short)(p.indices[k] + p.first - group.first);
        group.indexCount += (unsigned)p.indexCount;
    }
    return r;
}

bool Same(const GroundMerge &m, const Reference &r)
{
    if (m.groupCount != r.groups.size())
        return false;
    unsigned total = 0;
    for (unsigned g = 0; g < m.groupCount; g++) {
        const GroundMergeGroup &a = m.groups[g], &b = r.groups[g];
        if (a.buffer != b.buffer || a.texture != b.texture || a.cull != b.cull || a.lighting != b.lighting ||
            a.first != b.first || a.end != b.end || a.indexStart != b.indexStart || a.indexCount != b.indexCount)
            return false;
        total += a.indexCount;
    }
    for (unsigned i = 0; i < total; i++)
        if (m.indices[i] != r.indices[i])
            return false;
    return true;
}

// The parts of a stage: each has indices of its own, relative to its first
// vertex.
struct Stage {
    std::vector<std::vector<unsigned short>> indexData;
    std::vector<GroundMergePart> all;
    int meshes[64];

    Stage(unsigned partCount, int textures, int buffers)
    {
        indexData.resize(partCount);
        for (unsigned i = 0; i < partCount; i++) {
            const int vertices = 4 + (int)(Next() % 60);
            indexData[i].resize(3 * (1 + Next() % 20));
            for (unsigned short &index : indexData[i])
                index = (unsigned short)(Next() % (unsigned)vertices);
            GroundMergePart p;
            p.mesh = &meshes[Next() % 64];
            p.part = &indexData[i];
            p.indices = indexData[i].data();
            p.indexCount = (int)indexData[i].size();
            p.first = (int)(Next() % 3000);
            p.end = p.first + vertices;
            p.buffer = (int)(Next() % (unsigned)buffers);
            p.texture = (int)(Next() % (unsigned)textures);
            p.cull = 1 + (int)(Next() % 2);
            p.lighting = (int)(Next() % 3) - 1;
            all.push_back(p);
        }
    }

    // Some parts of the stage, in the order of the stage: a view of it.
    std::vector<GroundMergePart> View(unsigned share) const
    {
        std::vector<GroundMergePart> view;
        for (const GroundMergePart &p : all)
            if (Next() % 100 < share)
                view.push_back(p);
        return view;
    }
};

GroundMerge s_merge;

} // namespace

int main()
{
    Stage stage(600, 40, 3);
    GroundMerge_Forget(&s_merge);
    s_merge.made = s_merge.kept = 0;

    // A view: the merge is made. The same view again: it is kept.
    std::vector<GroundMergePart> view = stage.View(50);
    REQUIRE(GroundMerge_Update(&s_merge, view.data(), (unsigned)view.size()));
    REQUIRE(s_merge.made == 1 && s_merge.kept == 0 && Same(s_merge, MergeAgain(view)));
    REQUIRE(GroundMerge_Update(&s_merge, view.data(), (unsigned)view.size()));
    REQUIRE(s_merge.made == 1 && s_merge.kept == 1 && Same(s_merge, MergeAgain(view)));

    // One part less, one more, two in another order: made again each time.
    std::vector<GroundMergePart> other = view;
    other.pop_back();
    REQUIRE(GroundMerge_Update(&s_merge, other.data(), (unsigned)other.size()));
    REQUIRE(s_merge.made == 2 && Same(s_merge, MergeAgain(other)));
    REQUIRE(GroundMerge_Update(&s_merge, view.data(), (unsigned)view.size()));
    REQUIRE(s_merge.made == 3 && Same(s_merge, MergeAgain(view)));
    other = view;
    std::swap(other[3], other[4]);
    REQUIRE(GroundMerge_Update(&s_merge, other.data(), (unsigned)other.size()));
    REQUIRE(s_merge.made == 4 && Same(s_merge, MergeAgain(other)));

    // A part that differs in one thing from the frame before: made again,
    // for each thing of a part.
    unsigned short otherIndices[90] = {};
    for (int field = 0; field < 10; field++) {
        REQUIRE(GroundMerge_Update(&s_merge, view.data(), (unsigned)view.size()));
        const unsigned madeBefore = s_merge.made;
        other = view;
        GroundMergePart &p = other[other.size() / 2];
        switch (field) {
        case 0: p.mesh = &stage; break;
        case 1: p.part = &stage; break;
        case 2: p.indices = otherIndices; break;
        case 3: p.indexCount -= 3; break;
        case 4: p.first -= 1; break;
        case 5: p.end += 1; break;
        case 6: p.buffer += 1; break;
        case 7: p.texture += 1; break;
        case 8: p.cull += 1; break;
        case 9: p.lighting += 1; break;
        }
        REQUIRE(GroundMerge_Update(&s_merge, other.data(), (unsigned)other.size()));
        REQUIRE(s_merge.made == madeBefore + 1 && Same(s_merge, MergeAgain(other)));
    }

    // After a stage went: made again, also for the same parts.
    REQUIRE(GroundMerge_Update(&s_merge, view.data(), (unsigned)view.size()));
    unsigned madeBefore = s_merge.made;
    GroundMerge_Forget(&s_merge);
    REQUIRE(GroundMerge_Update(&s_merge, view.data(), (unsigned)view.size()));
    REQUIRE(s_merge.made == madeBefore + 1 && Same(s_merge, MergeAgain(view)));

    // Many frames of views of some stages: a new view in some of them.
    for (int round = 0; round < 40; round++) {
        Stage s(1 + Next() % 900, 1 + (int)(Next() % 200), 1 + (int)(Next() % 4));
        GroundMerge_Forget(&s_merge);
        std::vector<GroundMergePart> now = s.View(30 + Next() % 70);
        for (int frame = 0; frame < 30; frame++) {
            const bool fresh = Next() % 4 == 0;
            const unsigned made = s_merge.made, kept = s_merge.kept;
            if (fresh)
                now = s.View(30 + Next() % 70);
            REQUIRE(GroundMerge_Update(&s_merge, now.data(), (unsigned)now.size()));
            REQUIRE(Same(s_merge, MergeAgain(now)));
            if (!fresh && frame > 0)
                REQUIRE(s_merge.kept == kept + 1 && s_merge.made == made);
        }
    }

    // No part: no group.
    REQUIRE(GroundMerge_Update(&s_merge, view.data(), 0) && s_merge.groupCount == 0);

    // More groups, more indices and more parts than the merge holds: no
    // merge, and the next good list is made.
    {
        std::vector<GroundMergePart> many;
        unsigned short three[3] = { 0, 1, 2 };
        for (int i = 0; i < kGroundMergeGroups + 1; i++) {
            const GroundMergePart p = { &stage, &stage, three, 3, i * 4, i * 4 + 3, 0, i, 1, 0 };
            many.push_back(p);
        }
        REQUIRE(!GroundMerge_Update(&s_merge, many.data(), (unsigned)many.size()));
        many.pop_back();
        REQUIRE(GroundMerge_Update(&s_merge, many.data(), (unsigned)many.size()));
        REQUIRE(s_merge.groupCount == kGroundMergeGroups && Same(s_merge, MergeAgain(many)));

        std::vector<unsigned short> big(kGroundMergeIndices / 2 + 3, 1);
        const GroundMergePart half = { &stage, &stage, big.data(), (int)big.size(), 0, 8, 0, 0, 1, 0 };
        std::vector<GroundMergePart> two = { half, half };
        REQUIRE(!GroundMerge_Update(&s_merge, two.data(), 2));
        REQUIRE(GroundMerge_Update(&s_merge, two.data(), 1) && Same(s_merge, MergeAgain({ half })));

        const GroundMergePart one = many[0];
        many.assign(kGroundMergeParts + 1, one);
        REQUIRE(!GroundMerge_Update(&s_merge, many.data(), (unsigned)many.size()));
        REQUIRE(GroundMerge_Update(&s_merge, view.data(), (unsigned)view.size()) && Same(s_merge, MergeAgain(view)));
    }

    printf("ok\n");
    return 0;
}
