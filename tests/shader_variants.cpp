// The GXM fragment variant table (render/shader_variants_gen.h) against the
// key list it is generated from (render/shaders/cg/variants.txt): one row
// per fragment key and alpha test form, every key and triple covered, and
// no program that can discard drawn with blending. No GXM: the table only.

#include "port/gfx.h"
#include "render/shader_variants_gen.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "failed line %d: %s\n", __LINE__, #x); return 1; } } while (0)

namespace {

const uint64_t kAlphaTestBit = 1ull << 28;
const uint64_t kFadeBit = 1ull << 32;
const int kVariantCount = (int)(sizeof(kGxmFragmentVariants) / sizeof(kGxmFragmentVariants[0]));
const int kTripleCount = (int)(sizeof(kGxmPipelineTriples) / sizeof(kGxmPipelineTriples[0]));

struct ListedTriple {
    uint64_t key;
    int srcBlend, destBlend;   // 0, 0: blending off
    bool transformed;
};

// An args line: a fragment key and an arguments key it draws with.
typedef std::pair<uint64_t, uint64_t> ListedArgs;

bool ReadVariants(const char *path, std::vector<uint64_t> &keys, std::vector<ListedTriple> &triples,
                  std::set<ListedArgs> &args)
{
    std::ifstream in(path);
    if (!in)
        return false;
    std::string line;
    while (std::getline(in, line)) {
        line = line.substr(0, line.find('#'));
        std::istringstream words(line);
        std::string kind, key, blend, program;
        if (!(words >> kind))
            continue;
        words >> key;
        if (kind == "fragment") {
            keys.push_back(strtoull(key.c_str(), nullptr, 16));
        } else if (kind == "triple") {
            words >> blend >> program;
            ListedTriple t = { strtoull(key.c_str(), nullptr, 16), 0, 0, program == "tl" };
            if (blend != "off" && sscanf(blend.c_str(), "%d/%d", &t.srcBlend, &t.destBlend) != 2)
                return false;
            triples.push_back(t);
        } else if (kind == "args") {
            std::string value;
            words >> value;
            args.insert({ strtoull(key.c_str(), nullptr, 16), strtoull(value.c_str(), nullptr, 16) });
        }
    }
    return true;
}

// The form the executor picks: discard without blending, colour and alpha 0 with.
int ExpectedForm(uint64_t key, bool blend)
{
    if ((key & kAlphaTestBit) == 0)
        return 0;
    return blend ? 2 : 1;
}

bool HasSlot(const GxmFragmentVariant &v, int slot)
{
    for (int8_t unit : v.units)
        if (unit == slot)
            return true;
    return false;
}

} // namespace

int main(int argc, char **argv)
{
    REQUIRE(argc == 2);
    std::vector<uint64_t> keys;
    std::vector<ListedTriple> triples;
    std::set<ListedArgs> args;
    REQUIRE(ReadVariants(argv[1], keys, triples, args));
    REQUIRE(!keys.empty() && !triples.empty());

    // One general row per key and form, and only keys of the list. A row of
    // an arguments key is one of the list, has its general row, and its
    // compare form is on exactly with the alpha test.
    std::set<std::pair<uint64_t, int>> rows;
    std::set<std::pair<std::pair<uint64_t, int>, uint64_t>> made;
    for (const GxmFragmentVariant &v : kGxmFragmentVariants) {
        if (v.args == GXM_ARGS_GENERAL) {
            REQUIRE(rows.insert({ v.key, v.alphaTest }).second);
        } else {
            REQUIRE(made.insert({ { v.key, v.alphaTest }, v.args }).second);
            REQUIRE(args.count({ v.key, v.args }) == 1);
            REQUIRE(((v.args >> 40) != 0) == ((v.key & kAlphaTestBit) != 0));
        }
        REQUIRE(std::set<uint64_t>(keys.begin(), keys.end()).count(v.key) == 1);
        REQUIRE(((v.key & kAlphaTestBit) != 0) == (v.alphaTest != 0));
        REQUIRE(v.gxp == nullptr);
        // Units are packed from 0, each slot read once, the backdrop exactly
        // when the key has the distance fade.
        bool ended = false;
        std::set<int> slots;
        for (int8_t unit : v.units) {
            if (unit == GXM_SLOT_NONE) {
                ended = true;
                continue;
            }
            REQUIRE(!ended);
            REQUIRE(unit >= GXM_SLOT_TEX0 && unit <= GXM_SLOT_NATIVE1);
            REQUIRE(slots.insert(unit).second);
        }
        REQUIRE(HasSlot(v, GXM_SLOT_BACKDROP) == ((v.key & kFadeBit) != 0));
    }

    for (const auto &m : made)
        REQUIRE(rows.count(m.first) == 1);
    // Each listed arguments key has a row for each form of its key.
    for (const ListedArgs &a : args)
        for (const auto &row : rows)
            if (row.first == a.first)
                REQUIRE(made.count({ row, a.second }) == 1);

    // Each key with a blended form also has the discard form: the depth pass
    // of a blended draw that writes depth.
    for (const GxmFragmentVariant &v : kGxmFragmentVariants)
        if (v.alphaTest == 2)
            REQUIRE(rows.count({ v.key, 1 }) == 1);

    // Form 2 leaves a pixel as it was with a destination factor of one,
    // 1 - source colour or 1 - source alpha, and with no other.
    REQUIRE(Gxm_Form2Keeps(GFX_BLEND_ONE) && Gxm_Form2Keeps(GFX_BLEND_INVSRCCOLOR) &&
            Gxm_Form2Keeps(GFX_BLEND_INVSRCALPHA));
    REQUIRE(!Gxm_Form2Keeps(GFX_BLEND_ZERO) && !Gxm_Form2Keeps(GFX_BLEND_SRCALPHA) &&
            !Gxm_Form2Keeps(GFX_BLEND_DESTCOLOR));

    // Every listed key has a row.
    for (uint64_t key : keys) {
        bool found = false;
        for (const GxmFragmentVariant &v : kGxmFragmentVariants)
            found = found || v.key == key;
        REQUIRE(found);
    }

    // Every listed triple has its row in the table, on the variant of its
    // form; blending never meets a program that can discard.
    REQUIRE(kTripleCount == (int)triples.size());
    for (const ListedTriple &t : triples) {
        bool blend = t.srcBlend != 0;
        bool found = false;
        for (const GxmPipelineTriple &g : kGxmPipelineTriples) {
            REQUIRE(g.variant >= 0 && g.variant < kVariantCount);
            const GxmFragmentVariant &v = kGxmFragmentVariants[g.variant];
            REQUIRE(v.args == GXM_ARGS_GENERAL);
            REQUIRE(!(g.srcBlend != 0 && v.alphaTest == 1));
            if (v.key == t.key && g.srcBlend == t.srcBlend && g.destBlend == t.destBlend &&
                g.transformed == t.transformed) {
                REQUIRE(v.alphaTest == ExpectedForm(t.key, blend));
                found = true;
            }
        }
        REQUIRE(found);
    }

    // A draw too near for the distance fade draws with the key without it:
    // each triple and args line of a fade key has that key's line too.
    for (const ListedTriple &t : triples) {
        if ((t.key & kFadeBit) == 0)
            continue;
        bool found = false;
        for (const ListedTriple &o : triples)
            found = found || (o.key == (t.key & ~kFadeBit) && o.srcBlend == t.srcBlend && o.destBlend == t.destBlend &&
                              o.transformed == t.transformed);
        REQUIRE(found);
    }
    for (const ListedArgs &a : args)
        if ((a.first & kFadeBit) != 0)
            REQUIRE(args.count({ a.first & ~kFadeBit, a.second }) == 1);

    printf("shader_variants: %d variants of %d keys, %d triples\n", kVariantCount, (int)keys.size(), kTripleCount);
    return 0;
}
