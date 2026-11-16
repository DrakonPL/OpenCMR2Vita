// The merge of the ground parts of a frame, kept from frame to frame
// (game/GroundMerge.h): the same groups and indices as a merge that is made
// again each time, kept while the parts are the same, made again when a
// part differs in anything. The parts before the start of the distance fade
// have groups of their own, and each group has the reach of its parts as
// they are now.

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

// sides: the parts as they were when the groups were made (the side of the
// fade of each), where the merge was kept since. The reaches are those of
// parts.
Reference MergeAgain(const std::vector<GroundMergePart> &parts, const std::vector<GroundMergePart> *sides = nullptr)
{
    Reference r;
    std::vector<unsigned> partGroup;
    for (size_t i = 0; i < parts.size(); i++) {
        const GroundMergePart &p = parts[i];
        const int beforeFade = sides != nullptr ? (*sides)[i].beforeFade : p.beforeFade;
        unsigned g;
        for (g = 0; g < r.groups.size(); g++) {
            const GroundMergeGroup &group = r.groups[g];
            if (group.buffer == p.buffer && group.texture == p.texture && group.cull == p.cull &&
                group.lighting == p.lighting && group.beforeFade == beforeFade)
                break;
        }
        if (g == r.groups.size()) {
            const GroundMergeGroup group = { p.buffer, p.texture, p.cull, p.lighting, p.first, p.end, 0, 0,
                                             beforeFade, 0.0f };
            r.groups.push_back(group);
        }
        GroundMergeGroup &group = r.groups[g];
        group.first = p.first < group.first ? p.first : group.first;
        group.end = p.end > group.end ? p.end : group.end;
        group.indexCount += (unsigned)p.indexCount;
        // The farthest part, and not known where one part is not.
        if (group.reach >= 0.0f)
            group.reach = p.reach < 0.0f ? -1.0f : p.reach > group.reach ? p.reach : group.reach;
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
            a.first != b.first || a.end != b.end || a.indexStart != b.indexStart || a.indexCount != b.indexCount ||
            a.beforeFade != b.beforeFade || a.reach != b.reach)
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
            // The fade starts at 400. Some parts have no reach.
            p.reach = Next() % 10 == 0 ? -1.0f : (float)(Next() % 8000) * 0.1f;
            p.beforeFade = p.reach >= 0.0f && p.reach < 400.0f ? 1 : 0;
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
    const unsigned generation = s_merge.generation;

    // A view: the merge is made. The same view again: it is kept, with the
    // number of its indices (the copy of an index buffer stays good).
    std::vector<GroundMergePart> view = stage.View(50);
    REQUIRE(GroundMerge_Update(&s_merge, view.data(), (unsigned)view.size()));
    REQUIRE(s_merge.made == 1 && s_merge.kept == 0 && Same(s_merge, MergeAgain(view)));
    REQUIRE(s_merge.generation == generation + 1);
    unsigned indexCount = 0;
    for (const GroundMergePart &p : view)
        indexCount += (unsigned)p.indexCount;
    REQUIRE(GroundMerge_IndexCount(&s_merge) == indexCount);
    REQUIRE(GroundMerge_Update(&s_merge, view.data(), (unsigned)view.size()));
    REQUIRE(s_merge.made == 1 && s_merge.kept == 1 && Same(s_merge, MergeAgain(view)));
    REQUIRE(s_merge.generation == generation + 1);

    // One part less, one more, two in another order: made again each time.
    std::vector<GroundMergePart> other = view;
    other.pop_back();
    REQUIRE(GroundMerge_Update(&s_merge, other.data(), (unsigned)other.size()));
    REQUIRE(s_merge.made == 2 && Same(s_merge, MergeAgain(other)) && s_merge.generation == generation + 2);
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

    // The parts on the two sides of the fade's start are groups of their
    // own, and a group has the reach of its farthest part: not known where
    // one of its parts has none.
    {
        unsigned short three[3] = { 0, 1, 2 };
        const GroundMergePart a = { &stage, &stage, three, 3, 0, 3, 0, 5, 1, 0, 1, 120.0f };
        const GroundMergePart b = { &stage, &stage, three, 3, 8, 11, 0, 5, 1, 0, 0, 480.0f };
        const GroundMergePart c = { &stage, &stage, three, 3, 16, 19, 0, 5, 1, 0, 1, 250.0f };
        const GroundMergePart d = { &stage, &stage, three, 3, 24, 27, 0, 5, 1, 0, 0, -1.0f };
        std::vector<GroundMergePart> four = { a, b, c, d };
        REQUIRE(GroundMerge_Update(&s_merge, four.data(), 4) && s_merge.groupCount == 2);
        REQUIRE(s_merge.groups[0].beforeFade == 1 && s_merge.groups[0].reach == 250.0f);
        REQUIRE(s_merge.groups[0].first == 0 && s_merge.groups[0].end == 19 && s_merge.groups[0].indexCount == 6);
        REQUIRE(s_merge.groups[1].beforeFade == 0 && s_merge.groups[1].reach == -1.0f);
        REQUIRE(s_merge.groups[1].first == 8 && s_merge.groups[1].end == 27 && s_merge.groups[1].indexCount == 6);
        REQUIRE(Same(s_merge, MergeAgain(four)));
        // The view moves: the parts are the same, so the groups stay, with
        // the reaches of now. A part of the near group that is far now
        // makes its group far for the renderer, with no new groups.
        const std::vector<GroundMergePart> made = four;
        const unsigned madeBefore = s_merge.made;
        four[0].reach = 130.0f;
        four[1].reach = 300.0f;
        four[1].beforeFade = 1;
        four[2].reach = 410.0f;
        four[2].beforeFade = 0;
        four[3].reach = 600.0f;
        REQUIRE(GroundMerge_Update(&s_merge, four.data(), 4) && s_merge.made == madeBefore);
        REQUIRE(s_merge.groupCount == 2 && s_merge.groups[0].beforeFade == 1 && s_merge.groups[0].reach == 410.0f);
        REQUIRE(s_merge.groups[1].reach == 600.0f && Same(s_merge, MergeAgain(four, &made)));
        // Made again (a part goes): the groups by the sides of now.
        four.pop_back();
        REQUIRE(GroundMerge_Update(&s_merge, four.data(), 3) && s_merge.made == madeBefore + 1);
        REQUIRE(s_merge.groupCount == 2 && s_merge.groups[0].reach == 300.0f && s_merge.groups[0].indexCount == 6);
        REQUIRE(s_merge.groups[1].reach == 410.0f && Same(s_merge, MergeAgain(four)));
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
        std::vector<GroundMergePart> sides = now;
        for (int frame = 0; frame < 30; frame++) {
            const bool fresh = Next() % 4 == 0;
            const unsigned made = s_merge.made, kept = s_merge.kept;
            if (fresh) {
                now = s.View(30 + Next() % 70);
            } else {
                // The view moves: other reaches, and other sides of the fade.
                for (GroundMergePart &p : now) {
                    p.reach = Next() % 10 == 0 ? -1.0f : (float)(Next() % 8000) * 0.1f;
                    p.beforeFade = p.reach >= 0.0f && p.reach < 400.0f ? 1 : 0;
                }
            }
            if (fresh || frame == 0)
                sides = now;
            REQUIRE(GroundMerge_Update(&s_merge, now.data(), (unsigned)now.size()));
            REQUIRE(Same(s_merge, MergeAgain(now, &sides)));
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
