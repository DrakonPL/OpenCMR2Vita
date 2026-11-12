// The GXM fragment variant table (render/shader_variants_gen.h) against the
// key list it is generated from (render/shaders/cg/variants.txt): one row
// per fragment key and alpha test form, every key and triple covered, and
// no program that can discard drawn with blending. No GXM: the table only.

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

bool ReadVariants(const char *path, std::vector<uint64_t> &keys, std::vector<ListedTriple> &triples)
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
    REQUIRE(ReadVariants(argv[1], keys, triples));
    REQUIRE(!keys.empty() && !triples.empty());

    // One row per key and form, and only keys of the list.
    std::set<std::pair<uint64_t, int>> rows;
    for (const GxmFragmentVariant &v : kGxmFragmentVariants) {
        REQUIRE(rows.insert({ v.key, v.alphaTest }).second);
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
            REQUIRE(!(g.srcBlend != 0 && v.alphaTest == 1));
            if (v.key == t.key && g.srcBlend == t.srcBlend && g.destBlend == t.destBlend &&
                g.transformed == t.transformed) {
                REQUIRE(v.alphaTest == ExpectedForm(t.key, blend));
                found = true;
            }
        }
        REQUIRE(found);
    }

    printf("shader_variants: %d variants of %d keys, %d triples\n", kVariantCount, (int)keys.size(), kTripleCount);
    return 0;
}
