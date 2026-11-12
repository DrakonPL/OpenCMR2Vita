/*
 * Bink video decoder
 * Copyright (c) 2009 Konstantin Shishkov
 * Copyright (C) 2011 Peter Ross <pross@xvid.org>
 * Audio format reference: Copyright (c) 2007-2011 Peter Ross,
 * Copyright (c) 2009 Daniel Verkamp (libavcodec/binkaudio.c).
 *
 * Adapted for OpenCMR2 from FFmpeg n6.1 (libavcodec/bink.c).
 * Standalone BIKi implementation; no FFmpeg library dependency.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "bink_decoder.h"
#include "bink_tables.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <complex>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <functional>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace Bink {
namespace {
void Require(bool valid, const char *message) {
    if (!valid) throw std::runtime_error(message);
}

double NowMs() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// A thread that runs one job at a time for the decoder (Decoder::SetWorkers).
// A job keeps to a small stack: the Vita gives a thread 32 KB.
class Worker {
public:
    Worker() : thread([this] { Run(); }) {}
    ~Worker() {
        { std::lock_guard<std::mutex> lock(mutex); quit = true; }
        wake.notify_all();
        thread.join();
    }
    void Start(std::function<void()> next) {
        std::lock_guard<std::mutex> lock(mutex);
        job = std::move(next);
        failure = nullptr;
        busy = true;
        wake.notify_all();
    }
    // Waits for the job; its exception comes out here.
    void Wait() {
        std::unique_lock<std::mutex> lock(mutex);
        wake.wait(lock, [this] { return !busy; });
        std::exception_ptr error = failure;
        failure = nullptr;
        if (error) std::rethrow_exception(error);
    }
    // For a thread that is already throwing.
    void WaitQuietly() {
        std::unique_lock<std::mutex> lock(mutex);
        wake.wait(lock, [this] { return !busy; });
        failure = nullptr;
    }
private:
    void Run() {
        std::unique_lock<std::mutex> lock(mutex);
        for (;;) {
            wake.wait(lock, [this] { return busy || quit; });
            if (quit) return;
            std::function<void()> current = std::move(job);
            lock.unlock();
            std::exception_ptr error;
            try { current(); } catch (...) { error = std::current_exception(); }
            lock.lock();
            failure = error;
            busy = false;
            wake.notify_all();
        }
    }
    std::mutex mutex;
    std::condition_variable wake;
    std::function<void()> job;
    std::exception_ptr failure;
    bool busy = false, quit = false;
    std::thread thread;     // last: it starts after the members above exist
};

template<class T, size_t N> struct CheckedArray {
    T values[N] = {};
    T &operator[](int index) {
        Require(index >= 0 && size_t(index) < N, "coefficient list overflow");
        return values[index];
    }
};

static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "Bits loads little-endian words");

// Least significant bit first, as Bink writes them.
class Bits {
    const uint8_t *data;
    size_t bytes, size, position;

    // The next bits from position on, at least 57 of them; zero past the end.
    uint64_t Window() const {
        const size_t byte = position >> 3;
        uint64_t window = 0;
        std::memcpy(&window, data + byte, std::min<size_t>(8, bytes - byte));
        return window >> (position & 7);
    }
public:
    Bits(const uint8_t *data, size_t count, size_t startBit = 0)
        : data(data), bytes(count), size(count * 8), position(startBit) {}
    size_t Left() const { return size - position; }
    size_t Position() const { return position; }
    uint32_t Read(unsigned count) {
        Require(count <= 32 && count <= Left(), "truncated Bink bitstream");
        if (count == 0) return 0;
        const uint32_t value = uint32_t(Window() & (0xffffffffu >> (32 - count)));
        position += count;
        return value;
    }
    // Up to 25 bits, zero past the end.
    uint32_t Peek(unsigned count) const {
        return position < size ? uint32_t(Window() & ((1u << count) - 1)) : 0;
    }
    void Skip(unsigned count) {
        Require(count <= Left(), "truncated Bink bitstream");
        position += count;
    }
    void Align() { Read(unsigned((-position) & 31)); }
};

int Log2(unsigned value) { int n = 0; while (value >>= 1) ++n; return n; }
uint32_t LE32(const uint8_t *p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

enum Sources {
    BINK_SRC_BLOCK_TYPES, BINK_SRC_SUB_BLOCK_TYPES, BINK_SRC_COLORS,
    BINK_SRC_PATTERN, BINK_SRC_X_OFF, BINK_SRC_Y_OFF,
    BINK_SRC_INTRA_DC, BINK_SRC_INTER_DC, BINK_SRC_RUN, BINK_NB_SRC
};
enum BlockTypes {
    SKIP_BLOCK, SCALED_BLOCK, MOTION_BLOCK, RUN_BLOCK, RESIDUE_BLOCK,
    INTRA_BLOCK, FILL_BLOCK, INTER_BLOCK, PATTERN_BLOCK, RAW_BLOCK
};
struct Tree { unsigned vlc_num; uint8_t syms[16]; };
struct Bundle {
    int len;
    Tree tree;
    uint8_t *data, *data_end, *cur_dec, *cur_ptr;
};
struct Plane {
    int stride = 0, rows = 0;
    std::vector<uint8_t> current, previous;
    void Init(unsigned w, unsigned h) {
        stride = (w + 15) & ~15u;
        rows = (h + 15) & ~15u;
        current.assign(size_t(stride) * rows, 0);
        previous = current;
    }
};
struct VideoState {
    unsigned width = 0, height = 0;
    bool alpha = false;
    Plane planes[4];
    Bundle bundle[BINK_NB_SRC] = {};
    std::vector<uint8_t> bundleStorage;
    Tree col_high[16];
    int col_lastval;
    // withPlanes false: the bundles alone, for decoding another state's planes.
    void Init(unsigned w, unsigned h, bool hasAlpha, bool withPlanes = true) {
        width = w; height = h; alpha = hasAlpha;
        for (int i = 0; withPlanes && i < (alpha ? 4 : 3); ++i)
            planes[i].Init((i == 1 || i == 2) ? (w + 1) / 2 : w,
                           (i == 1 || i == 2) ? (h + 1) / 2 : h);
        size_t bytes = size_t((w + 7) / 8) * ((h + 7) / 8) * 64;
        bundleStorage.resize(bytes * BINK_NB_SRC);
        for (int i = 0; i < BINK_NB_SRC; ++i) {
            bundle[i].data = bundleStorage.data() + bytes * i;
            bundle[i].data_end = bundle[i].data + bytes;
        }
    }
};

// For each tree: the next 7 bits -> symbol + 16 * code length, 0 for no code.
// Shorter codes, then lower symbols, win, as in a search bit by bit.
struct HuffTables {
    uint8_t entry[16][128] = {};
    HuffTables() {
        for (int vlc = 0; vlc < 16; ++vlc)
            for (unsigned length = 7; length >= 1; --length)
                for (int symbol = 15; symbol >= 0; --symbol) {
                    const unsigned code = bink_tree_bits[vlc][symbol];
                    if (bink_tree_lens[vlc][symbol] != length || code >> length)
                        continue;
                    for (unsigned rest = 0; rest < (128u >> length); ++rest)
                        entry[vlc][code | rest << length] = uint8_t(symbol + 16 * length);
                }
    }
};

uint8_t Huff(Bits *bits, const Tree &tree) {
    if (!tree.vlc_num)
        return tree.syms[bits->Read(4)];
    static const HuffTables tables;
    const uint8_t entry = tables.entry[tree.vlc_num][bits->Peek(7)];
    Require(entry != 0, "invalid Bink Huffman code");
    bits->Skip(entry >> 4);
    return tree.syms[entry & 15];
}
#define GET_HUFF(gb, tree) Huff(gb, tree)

static void init_lengths(VideoState *c, int width, int bw)
{
    width = ((width + 7) & ~7);

    c->bundle[BINK_SRC_BLOCK_TYPES].len = Log2((width >> 3) + 511) + 1;

    c->bundle[BINK_SRC_SUB_BLOCK_TYPES].len = Log2((width >> 4) + 511) + 1;

    c->bundle[BINK_SRC_COLORS].len = Log2(bw*64 + 511) + 1;

    c->bundle[BINK_SRC_INTRA_DC].len =
    c->bundle[BINK_SRC_INTER_DC].len =
    c->bundle[BINK_SRC_X_OFF].len =
    c->bundle[BINK_SRC_Y_OFF].len = Log2((width >> 3) + 511) + 1;

    c->bundle[BINK_SRC_PATTERN].len = Log2((bw << 3) + 511) + 1;

    c->bundle[BINK_SRC_RUN].len = Log2(bw*48 + 511) + 1;
}

static void merge(Bits *gb, uint8_t *dst, uint8_t *src, int size)
{
    uint8_t *src2 = src + size;
    int size2 = size;

    do {
        if (!gb->Read(1)) {
            *dst++ = *src++;
            size--;
        } else {
            *dst++ = *src2++;
            size2--;
        }
    } while (size && size2);

    while (size--)
        *dst++ = *src++;
    while (size2--)
        *dst++ = *src2++;
}

static int read_tree(Bits *gb, Tree *tree)
{
    uint8_t tmp1[16] = { 0 }, tmp2[16], *in = tmp1, *out = tmp2;
    int i, t, len;

    if (gb->Left() < 4)
        return -1;

    tree->vlc_num = gb->Read(4);
    if (!tree->vlc_num) {
        for (i = 0; i < 16; i++)
            tree->syms[i] = i;
        return 0;
    }
    if (gb->Read(1)) {
        len = gb->Read(3);
        for (i = 0; i <= len; i++) {
            tree->syms[i] = gb->Read(4);
            tmp1[tree->syms[i]] = 1;
        }
        for (i = 0; i < 16 && len < 16 - 1; i++)
            if (!tmp1[i])
                tree->syms[++len] = i;
    } else {
        len = gb->Read(2);
        for (i = 0; i < 16; i++)
            in[i] = i;
        for (i = 0; i <= len; i++) {
            int size = 1 << i;
            for (t = 0; t < 16; t += size << 1)
                merge(gb, out + t, in + t, size);
            std::swap(in, out);
        }
        memcpy(tree->syms, in, 16);
    }
    return 0;
}

static int read_bundle(Bits *gb, VideoState *c, int bundle_num)
{
    int i;

    if (bundle_num == BINK_SRC_COLORS) {
        for (i = 0; i < 16; i++) {
            int ret = read_tree(gb, &c->col_high[i]);
            if (ret < 0)
                return ret;
        }
        c->col_lastval = 0;
    }
    if (bundle_num != BINK_SRC_INTRA_DC && bundle_num != BINK_SRC_INTER_DC) {
        int ret = read_tree(gb, &c->bundle[bundle_num].tree);
        if (ret < 0)
            return ret;
    }
    c->bundle[bundle_num].cur_dec =
    c->bundle[bundle_num].cur_ptr = c->bundle[bundle_num].data;

    return 0;
}

#define CHECK_READ_VAL(gb, b, t) \
    if (!b->cur_dec || (b->cur_dec > b->cur_ptr)) \
        return 0; \
    t = gb->Read(b->len); \
    if (!t) { \
        b->cur_dec = NULL; \
        return 0; \
    } \

static int read_runs(Bits *gb, Bundle *b)
{
    int t, v;
    const uint8_t *dec_end;

    CHECK_READ_VAL(gb, b, t);
    if (t > b->data_end - b->cur_dec) {
        return -1;
    }
    dec_end = b->cur_dec + t;
    if (gb->Left() < 1)
        return -1;
    if (gb->Read(1)) {
        v = gb->Read(4);
        memset(b->cur_dec, v, t);
        b->cur_dec += t;
    } else {
        while (b->cur_dec < dec_end)
            *b->cur_dec++ = GET_HUFF(gb, b->tree);
    }
    return 0;
}

static int read_motion_values(Bits *gb, Bundle *b)
{
    int t, sign, v;
    const uint8_t *dec_end;

    CHECK_READ_VAL(gb, b, t);
    if (t > b->data_end - b->cur_dec) {
        return -1;
    }
    dec_end = b->cur_dec + t;
    if (gb->Left() < 1)
        return -1;
    if (gb->Read(1)) {
        v = gb->Read(4);
        if (v) {
            sign = -gb->Read(1);
            v = (v ^ sign) - sign;
        }
        memset(b->cur_dec, v, t);
        b->cur_dec += t;
    } else {
        while (b->cur_dec < dec_end) {
            v = GET_HUFF(gb, b->tree);
            if (v) {
                sign = -gb->Read(1);
                v = (v ^ sign) - sign;
            }
            *b->cur_dec++ = v;
        }
    }
    return 0;
}

static const uint8_t bink_rlelens[4] = { 4, 8, 12, 32 };

static int read_block_types(Bits *gb, Bundle *b)
{
    int t, v;
    int last = 0;
    const uint8_t *dec_end;

    CHECK_READ_VAL(gb, b, t);
    if (t > b->data_end - b->cur_dec) {
        return -1;
    }
    dec_end = b->cur_dec + t;
    if (gb->Left() < 1)
        return -1;
    if (gb->Read(1)) {
        v = gb->Read(4);
        memset(b->cur_dec, v, t);
        b->cur_dec += t;
    } else {
        while (b->cur_dec < dec_end) {
            v = GET_HUFF(gb, b->tree);
            if (v < 12) {
                last = v;
                *b->cur_dec++ = v;
            } else {
                int run = bink_rlelens[v - 12];

                if (dec_end - b->cur_dec < run)
                    return -1;
                memset(b->cur_dec, last, run);
                b->cur_dec += run;
            }
        }
    }
    return 0;
}

static int read_patterns(Bits *gb, Bundle *b)
{
    int t, v;
    const uint8_t *dec_end;

    CHECK_READ_VAL(gb, b, t);
    if (t > b->data_end - b->cur_dec) {
        return -1;
    }
    dec_end = b->cur_dec + t;
    while (b->cur_dec < dec_end) {
        if (gb->Left() < 2)
            return -1;
        v  = GET_HUFF(gb, b->tree);
        v |= GET_HUFF(gb, b->tree) << 4;
        *b->cur_dec++ = v;
    }

    return 0;
}

static int read_colors(Bits *gb, Bundle *b, VideoState *c)
{
    int t, v;
    const uint8_t *dec_end;

    CHECK_READ_VAL(gb, b, t);
    if (t > b->data_end - b->cur_dec) {
        return -1;
    }
    dec_end = b->cur_dec + t;
    if (gb->Left() < 1)
        return -1;
    if (gb->Read(1)) {
        c->col_lastval = GET_HUFF(gb, c->col_high[c->col_lastval]);
        v = GET_HUFF(gb, b->tree);
        v = (c->col_lastval << 4) | v;
        memset(b->cur_dec, v, t);
        b->cur_dec += t;
    } else {
        while (b->cur_dec < dec_end) {
            if (gb->Left() < 2)
                return -1;
            c->col_lastval = GET_HUFF(gb, c->col_high[c->col_lastval]);
            v = GET_HUFF(gb, b->tree);
            v = (c->col_lastval << 4) | v;
            *b->cur_dec++ = v;
        }
    }
    return 0;
}

static int read_dcs(Bits *gb, Bundle *b,
                    int start_bits, int has_sign)
{
    int i, j, len, len2, bsize, sign, v, v2;
    int16_t *dst     = (int16_t*)b->cur_dec;
    int16_t *dst_end = (int16_t*)b->data_end;

    CHECK_READ_VAL(gb, b, len);
    if (gb->Left() < unsigned(start_bits - has_sign))
        return -1;
    v = gb->Read(start_bits - has_sign);
    if (v && has_sign) {
        sign = -gb->Read(1);
        v = (v ^ sign) - sign;
    }
    if (dst_end - dst < 1)
        return -1;
    *dst++ = v;
    len--;
    for (i = 0; i < len; i += 8) {
        len2 = std::min(len - i, 8);
        if (dst_end - dst < len2)
            return -1;
        bsize = gb->Read(4);
        if (bsize) {
            for (j = 0; j < len2; j++) {
                v2 = gb->Read(bsize);
                if (v2) {
                    sign = -gb->Read(1);
                    v2 = (v2 ^ sign) - sign;
                }
                v += v2;
                *dst++ = v;
                if (v < -32768 || v > 32767) {
                    return -1;
                }
            }
        } else {
            for (j = 0; j < len2; j++)
                *dst++ = v;
        }
    }

    b->cur_dec = (uint8_t*)dst;
    return 0;
}

static inline int get_value(VideoState *c, int bundle)
{
    int ret;
    Bundle &b = c->bundle[bundle];
    const int bytes = (bundle == BINK_SRC_INTRA_DC || bundle == BINK_SRC_INTER_DC) ? 2 : 1;
    Require(b.cur_dec && b.cur_dec - b.cur_ptr >= bytes, "exhausted video bundle");

    if (bundle < BINK_SRC_X_OFF || bundle == BINK_SRC_RUN)
        return *c->bundle[bundle].cur_ptr++;
    if (bundle == BINK_SRC_X_OFF || bundle == BINK_SRC_Y_OFF)
        return (int8_t)*c->bundle[bundle].cur_ptr++;
    ret = *(int16_t*)c->bundle[bundle].cur_ptr;
    c->bundle[bundle].cur_ptr += 2;
    return ret;
}

static int read_dct_coeffs(Bits *gb, int32_t block[64],
                           const uint8_t *scan, int *coef_count_,
                           int coef_idx[64], int q)
{
    CheckedArray<int, 128> coef_list;
    CheckedArray<int, 128> mode_list;
    int i, t, bits, ccoef, mode, sign;
    int list_start = 64, list_end = 64, list_pos;
    int coef_count = 0;
    int quant_idx;

    if (gb->Left() < 4)
        return -1;

    coef_list[list_end] = 4;  mode_list[list_end++] = 0;
    coef_list[list_end] = 24; mode_list[list_end++] = 0;
    coef_list[list_end] = 44; mode_list[list_end++] = 0;
    coef_list[list_end] = 1;  mode_list[list_end++] = 3;
    coef_list[list_end] = 2;  mode_list[list_end++] = 3;
    coef_list[list_end] = 3;  mode_list[list_end++] = 3;

    for (bits = gb->Read(4) - 1; bits >= 0; bits--) {
        list_pos = list_start;
        while (list_pos < list_end) {
            if (!(mode_list[list_pos] | coef_list[list_pos]) || !gb->Read(1)) {
                list_pos++;
                continue;
            }
            ccoef = coef_list[list_pos];
            mode  = mode_list[list_pos];
            switch (mode) {
            case 0:
                coef_list[list_pos] = ccoef + 4;
                mode_list[list_pos] = 1;
            case 2:
                if (mode == 2) {
                    coef_list[list_pos]   = 0;
                    mode_list[list_pos++] = 0;
                }
                for (i = 0; i < 4; i++, ccoef++) {
                    if (gb->Read(1)) {
                        coef_list[--list_start] = ccoef;
                        mode_list[  list_start] = 3;
                    } else {
                        if (!bits) {
                            t = 1 - (gb->Read(1) << 1);
                        } else {
                            t = gb->Read(bits) | 1 << bits;
                            sign = -gb->Read(1);
                            t = (t ^ sign) - sign;
                        }
                        Require(ccoef >= 0 && ccoef < 64 && coef_count < 64, "DCT coefficient overflow");
                        block[scan[ccoef]] = t;
                        coef_idx[coef_count++] = ccoef;
                    }
                }
                break;
            case 1:
                mode_list[list_pos] = 2;
                for (i = 0; i < 3; i++) {
                    ccoef += 4;
                    coef_list[list_end]   = ccoef;
                    mode_list[list_end++] = 2;
                }
                break;
            case 3:
                if (!bits) {
                    t = 1 - (gb->Read(1) << 1);
                } else {
                    t = gb->Read(bits) | 1 << bits;
                    sign = -gb->Read(1);
                    t = (t ^ sign) - sign;
                }
                Require(ccoef >= 0 && ccoef < 64 && coef_count < 64, "DCT coefficient overflow");
                block[scan[ccoef]] = t;
                coef_idx[coef_count++] = ccoef;
                coef_list[list_pos]   = 0;
                mode_list[list_pos++] = 0;
                break;
            }
        }
    }

    if (q == -1) {
        quant_idx = gb->Read(4);
    } else {
        quant_idx = q;
        if (unsigned(quant_idx) > 15) {
            return -1;
        }
    }

    *coef_count_ = coef_count;

    return quant_idx;
}

static void unquantize_dct_coeffs(int32_t block[64], const int32_t quant[64],
                                  int coef_count, int coef_idx[64],
                                  const uint8_t *scan)
{
    int i;
    block[0] = int32_t(uint32_t(block[0]) * uint32_t(quant[0])) >> 11;
    for (i = 0; i < coef_count; i++) {
        int idx = coef_idx[i];
        block[scan[idx]] = int32_t(uint32_t(block[scan[idx]]) * uint32_t(quant[idx])) >> 11;
    }
}

static int read_residue(Bits *gb, int16_t block[64], int masks_count)
{
    CheckedArray<int, 128> coef_list;
    CheckedArray<int, 128> mode_list;
    int i, sign, mask, ccoef, mode;
    int list_start = 64, list_end = 64, list_pos;
    CheckedArray<int, 64> nz_coeff;
    int nz_coeff_count = 0;

    coef_list[list_end] =  4; mode_list[list_end++] = 0;
    coef_list[list_end] = 24; mode_list[list_end++] = 0;
    coef_list[list_end] = 44; mode_list[list_end++] = 0;
    coef_list[list_end] =  0; mode_list[list_end++] = 2;

    for (mask = 1 << gb->Read(3); mask; mask >>= 1) {
        for (i = 0; i < nz_coeff_count; i++) {
            if (!gb->Read(1))
                continue;
            if (block[nz_coeff[i]] < 0)
                block[nz_coeff[i]] -= mask;
            else
                block[nz_coeff[i]] += mask;
            masks_count--;
            if (masks_count < 0)
                return 0;
        }
        list_pos = list_start;
        while (list_pos < list_end) {
            if (!(coef_list[list_pos] | mode_list[list_pos]) || !gb->Read(1)) {
                list_pos++;
                continue;
            }
            ccoef = coef_list[list_pos];
            mode  = mode_list[list_pos];
            switch (mode) {
            case 0:
                coef_list[list_pos] = ccoef + 4;
                mode_list[list_pos] = 1;
            case 2:
                if (mode == 2) {
                    coef_list[list_pos]   = 0;
                    mode_list[list_pos++] = 0;
                }
                for (i = 0; i < 4; i++, ccoef++) {
                    if (gb->Read(1)) {
                        coef_list[--list_start] = ccoef;
                        mode_list[  list_start] = 3;
                    } else {
                        Require(ccoef >= 0 && ccoef < 64 && nz_coeff_count < 64, "residue coefficient overflow");
                        nz_coeff[nz_coeff_count++] = bink_scan[ccoef];
                        sign = -gb->Read(1);
                        block[bink_scan[ccoef]] = (mask ^ sign) - sign;
                        masks_count--;
                        if (masks_count < 0)
                            return 0;
                    }
                }
                break;
            case 1:
                mode_list[list_pos] = 2;
                for (i = 0; i < 3; i++) {
                    ccoef += 4;
                    coef_list[list_end]   = ccoef;
                    mode_list[list_end++] = 2;
                }
                break;
            case 3:
                Require(ccoef >= 0 && ccoef < 64 && nz_coeff_count < 64, "residue coefficient overflow");
                nz_coeff[nz_coeff_count++] = bink_scan[ccoef];
                sign = -gb->Read(1);
                block[bink_scan[ccoef]] = (mask ^ sign) - sign;
                coef_list[list_pos]   = 0;
                mode_list[list_pos++] = 0;
                masks_count--;
                if (masks_count < 0)
                    return 0;
                break;
            }
        }
    }

    return 0;
}

#define A1  2896 /* (1/sqrt(2))<<12 */
#define A2  2217
#define A3  3784
#define A4 -5352

#define MUL(X,Y) ((int)((unsigned)(X) * (Y)) >> 11)

#define IDCT_TRANSFORM(dest,s0,s1,s2,s3,s4,s5,s6,s7,d0,d1,d2,d3,d4,d5,d6,d7,munge,src) {\
    const int a0 = (src)[s0] + (src)[s4]; \
    const int a1 = (src)[s0] - (src)[s4]; \
    const int a2 = (src)[s2] + (src)[s6]; \
    const int a3 = MUL(A1, (src)[s2] - (src)[s6]); \
    const int a4 = (src)[s5] + (src)[s3]; \
    const int a5 = (src)[s5] - (src)[s3]; \
    const int a6 = (src)[s1] + (src)[s7]; \
    const int a7 = (src)[s1] - (src)[s7]; \
    const int b0 = a4 + a6; \
    const int b1 = MUL(A3, a5 + a7); \
    const int b2 = MUL(A4, a5) - b0 + b1; \
    const int b3 = MUL(A1, a6 - a4) - b2; \
    const int b4 = MUL(A2, a7) + b3 - b1; \
    (dest)[d0] = munge(a0+a2   +b0); \
    (dest)[d1] = munge(a1+a3-a2+b2); \
    (dest)[d2] = munge(a1-a3+a2+b3); \
    (dest)[d3] = munge(a0-a2   -b4); \
    (dest)[d4] = munge(a0-a2   +b4); \
    (dest)[d5] = munge(a1-a3+a2-b3); \
    (dest)[d6] = munge(a1+a3-a2-b2); \
    (dest)[d7] = munge(a0+a2   -b0); \
}
/* end IDCT_TRANSFORM macro */

#define MUNGE_NONE(x) (x)
#define IDCT_COL(dest,src) IDCT_TRANSFORM(dest,0,8,16,24,32,40,48,56,0,8,16,24,32,40,48,56,MUNGE_NONE,src)

#define MUNGE_ROW(x) (((x) + 0x7F)>>8)
#define IDCT_ROW(dest,src) IDCT_TRANSFORM(dest,0,1,2,3,4,5,6,7,0,1,2,3,4,5,6,7,MUNGE_ROW,src)

static inline void bink_idct_col(int *dest, const int32_t *src)
{
    if ((src[8]|src[16]|src[24]|src[32]|src[40]|src[48]|src[56])==0) {
        dest[0]  =
        dest[8]  =
        dest[16] =
        dest[24] =
        dest[32] =
        dest[40] =
        dest[48] =
        dest[56] = src[0];
    } else {
        IDCT_COL(dest, src);
    }
}

static void bink_idct_c(int32_t *block)
{
    int i;
    int temp[64];

    for (i = 0; i < 8; i++)
        bink_idct_col(&temp[i], &block[i]);
    for (i = 0; i < 8; i++) {
        IDCT_ROW( (&block[8*i]), (&temp[8*i]) );
    }
}

static void bink_idct_add_c(uint8_t *dest, int linesize, int32_t *block)
{
    int i, j;

    bink_idct_c(block);
    for (i = 0; i < 8; i++, dest += linesize, block += 8)
        for (j = 0; j < 8; j++)
             dest[j] += block[j];
}

static void bink_idct_put_c(uint8_t *dest, int linesize, int32_t *block)
{
    int i;
    int temp[64];
    for (i = 0; i < 8; i++)
        bink_idct_col(&temp[i], &block[i]);
    for (i = 0; i < 8; i++) {
        IDCT_ROW( (&dest[i*linesize]), (&temp[8*i]) );
    }
}


void CopyBlock(uint8_t *dst, const uint8_t *src, int stride) {
    for (int y = 0; y < 8; ++y) std::memcpy(dst + y * stride, src + y * stride, 8);
}

// s gives the size and the bundles; plane may belong to another state.
void DecodePlane(VideoState &s, Plane &plane, Bits &bits, bool chroma) {
    int bw = (s.width + (chroma ? 15 : 7)) >> (chroma ? 4 : 3);
    int bh = (s.height + (chroma ? 15 : 7)) >> (chroma ? 4 : 3);
    init_lengths(&s, std::max(int(s.width >> chroma), 8), bw);
    for (int i = 0; i < BINK_NB_SRC; ++i)
        Require(read_bundle(&bits, &s, i) == 0, "invalid Bink bundle tree");
    auto value = [&](int source) { return get_value(&s, source); };
    for (int by = 0; by < bh; ++by) {
        Require(read_block_types(&bits, &s.bundle[BINK_SRC_BLOCK_TYPES]) == 0 &&
                read_block_types(&bits, &s.bundle[BINK_SRC_SUB_BLOCK_TYPES]) == 0 &&
                read_colors(&bits, &s.bundle[BINK_SRC_COLORS], &s) == 0 &&
                read_patterns(&bits, &s.bundle[BINK_SRC_PATTERN]) == 0 &&
                read_motion_values(&bits, &s.bundle[BINK_SRC_X_OFF]) == 0 &&
                read_motion_values(&bits, &s.bundle[BINK_SRC_Y_OFF]) == 0 &&
                read_dcs(&bits, &s.bundle[BINK_SRC_INTRA_DC], 11, 0) == 0 &&
                read_dcs(&bits, &s.bundle[BINK_SRC_INTER_DC], 11, 1) == 0 &&
                read_runs(&bits, &s.bundle[BINK_SRC_RUN]) == 0, "invalid Bink bundle");
        for (int bx = 0; bx < bw; ++bx) {
            int type = value(BINK_SRC_BLOCK_TYPES);
            if (type == SCALED_BLOCK && ((by & 1) || (bx & 1))) {
                Require(bx + 1 < bw, "scaled block outside plane");
                ++bx;
                continue;
            }
            bool scaled = type == SCALED_BLOCK;
            if (scaled) type = value(BINK_SRC_SUB_BLOCK_TYPES);
            Require(!scaled || type == RUN_BLOCK || type == INTRA_BLOCK || type == FILL_BLOCK ||
                    type == PATTERN_BLOCK || type == RAW_BLOCK, "invalid scaled block type");
            const int stride = plane.stride;
            uint8_t *target = plane.current.data() + by * 8 * stride + bx * 8;
            uint8_t scratch[64] = {};
            uint8_t *dst = scaled ? scratch : target;
            int pitch = scaled ? 8 : stride;
            auto motion = [&] {
                int x = bx * 8 + value(BINK_SRC_X_OFF);
                int y = by * 8 + value(BINK_SRC_Y_OFF);
                Require(x >= 0 && y >= 0 && x + 8 <= bw * 8 && y + 8 <= bh * 8,
                        "Bink motion vector outside plane");
                CopyBlock(dst, plane.previous.data() + y * stride + x, stride);
            };
            switch (type) {
            case SKIP_BLOCK:
                CopyBlock(dst, plane.previous.data() + by * 8 * stride + bx * 8, stride);
                break;
            case MOTION_BLOCK:
                motion();
                break;
            case RESIDUE_BLOCK: {
                motion();
                int16_t block[64] = {};
                Require(read_residue(&bits, block, bits.Read(7)) == 0, "invalid Bink residue");
                for (int y = 0; y < 8; ++y)
                    for (int x = 0; x < 8; ++x) dst[y * pitch + x] += block[y * 8 + x];
                break;
            }
            case INTRA_BLOCK:
            case INTER_BLOCK: {
                if (type == INTER_BLOCK) motion();
                int32_t block[64] = {};
                int count = 0, indices[64];
                block[0] = value(type == INTRA_BLOCK ? BINK_SRC_INTRA_DC : BINK_SRC_INTER_DC);
                int q = read_dct_coeffs(&bits, block, bink_scan, &count, indices, -1);
                Require(q >= 0 && q < 16, "invalid Bink DCT quantizer");
                unquantize_dct_coeffs(block, type == INTRA_BLOCK ? bink_intra_quant[q] : bink_inter_quant[q],
                                      count, indices, bink_scan);
                if (type == INTRA_BLOCK) bink_idct_put_c(dst, pitch, block);
                else bink_idct_add_c(dst, pitch, block);
                break;
            }
            case FILL_BLOCK: {
                uint8_t color = value(BINK_SRC_COLORS);
                for (int y = 0; y < 8; ++y) std::memset(dst + y * pitch, color, 8);
                break;
            }
            case PATTERN_BLOCK: {
                uint8_t colors[2];
                colors[0] = value(BINK_SRC_COLORS); colors[1] = value(BINK_SRC_COLORS);
                for (int y = 0; y < 8; ++y) {
                    unsigned pattern = value(BINK_SRC_PATTERN);
                    for (int x = 0; x < 8; ++x, pattern >>= 1) dst[y * pitch + x] = colors[pattern & 1];
                }
                break;
            }
            case RAW_BLOCK:
                for (int y = 0; y < 8; ++y)
                    for (int x = 0; x < 8; ++x) dst[y * pitch + x] = value(BINK_SRC_COLORS);
                break;
            case RUN_BLOCK: {
                const uint8_t *scan = bink_patterns[bits.Read(4)];
                int position = 0;
                while (position < 63) {
                    int count = value(BINK_SRC_RUN) + 1;
                    Require(count <= 64 - position, "Bink run outside block");
                    bool constant = bits.Read(1);
                    int color = constant ? value(BINK_SRC_COLORS) : 0;
                    for (int i = 0; i < count; ++i) {
                        unsigned pixel = scan[position++];
                        dst[(pixel >> 3) * pitch + (pixel & 7)] = constant ? color : value(BINK_SRC_COLORS);
                    }
                }
                if (position == 63) {
                    unsigned pixel = scan[position];
                    dst[(pixel >> 3) * pitch + (pixel & 7)] = value(BINK_SRC_COLORS);
                }
                break;
            }
            default: throw std::runtime_error("unknown Bink block type");
            }
            if (scaled) {
                Require(bx + 1 < bw && by + 1 < bh, "scaled block outside plane");
                for (int y = 0; y < 16; ++y)
                    for (int x = 0; x < 16; ++x) target[y * stride + x] = scratch[(y / 2) * 8 + x / 2];
                ++bx;
            }
        }
    }
    bits.Align();
}

uint8_t Clamp(int value) { return uint8_t(std::clamp(value, 0, 255)); }

// Rows y0 to y1 of the picture from the planes (BT.601, limited range).
void ConvertRows(const VideoState &s, uint8_t *bgrx, unsigned y0, unsigned y1) {
    const Plane &py = s.planes[0], &pu = s.planes[1], &pv = s.planes[2];
    for (unsigned y = y0; y < y1; ++y) {
        const uint8_t *lumaRow = py.current.data() + size_t(y) * py.stride;
        const uint8_t *uRow = pu.current.data() + size_t(y / 2) * pu.stride;
        const uint8_t *vRow = pv.current.data() + size_t(y / 2) * pv.stride;
        uint8_t *dst = bgrx + size_t(y) * s.width * 4;
        for (unsigned x = 0; x < s.width; ++x, dst += 4) {
            const int luma = 298 * std::max(int(lumaRow[x]) - 16, 0) + 128;
            const int u = int(uRow[x / 2]) - 128, v = int(vRow[x / 2]) - 128;
            dst[0] = Clamp((luma + 516 * u) >> 8);
            dst[1] = Clamp((luma - 100 * u - 208 * v) >> 8);
            dst[2] = Clamp((luma + 409 * v) >> 8);
            dst[3] = 255;
        }
    }
}

// The planes of a frame, then the picture. With a worker the chroma planes
// decode there while this thread decodes luma (BIKi gives the byte where luma
// ends), and each thread converts half of the rows. chromaState holds the
// worker's bundles.
void DecodeVideo(VideoState &s, VideoState &chromaState, Worker *worker, const uint8_t *data, size_t bytes,
                 Frame &frame, Timing &timing) {
    Bits bits(data, bytes);
    frame.bgrx.resize(size_t(s.width) * s.height * 4);
    if (worker == nullptr || s.alpha) {
        double start = NowMs();
        if (s.alpha) { bits.Read(32); DecodePlane(s, s.planes[3], bits, false); }
        bits.Read(32); // BIKi: the byte where the luma plane ends; planes are word aligned.
        DecodePlane(s, s.planes[0], bits, false);
        timing.luma = NowMs() - start;
        start = NowMs();
        DecodePlane(s, s.planes[2], bits, true); // BIKi stores V before U.
        DecodePlane(s, s.planes[1], bits, true);
        timing.chroma = NowMs() - start;
        start = NowMs();
        ConvertRows(s, frame.bgrx.data(), 0, s.height);
        timing.convert = NowMs() - start;
    } else {
        const uint32_t lumaEnd = bits.Read(32);
        Require(lumaEnd >= 4 && lumaEnd <= bytes, "invalid Bink luma plane size");
        Bits chroma(data, bytes, size_t(lumaEnd) * 8);
        worker->Start([&] {
            const double start = NowMs();
            DecodePlane(chromaState, s.planes[2], chroma, true);
            DecodePlane(chromaState, s.planes[1], chroma, true);
            timing.chroma = NowMs() - start;
        });
        try {
            const double start = NowMs();
            DecodePlane(s, s.planes[0], bits, false);
            Require(bits.Position() == size_t(lumaEnd) * 8, "Bink luma plane size mismatch");
            timing.luma = NowMs() - start;
        } catch (...) {
            worker->WaitQuietly();
            throw;
        }
        worker->Wait();
        const double start = NowMs();
        const unsigned half = s.height / 2;
        worker->Start([&] { ConvertRows(s, frame.bgrx.data(), half, s.height); });
        ConvertRows(s, frame.bgrx.data(), 0, half);
        worker->Wait();
        timing.convert = NowMs() - start;
    }
    for (Plane &plane : s.planes) plane.previous.swap(plane.current);
}

// Radix-2 complex FFT, unnormalised inverse. Bink uses only small fixed powers of two.
constexpr double Pi = 3.14159265358979323846;

// The factors of each stage of InverseFFT for n points: stage length L starts
// at L / 2 - 1. Each comes from repeated multiplication, as a running factor
// in the loop would, so the results do not change by a bit.
std::vector<std::complex<double>> FFTTwiddles(size_t n) {
    std::vector<std::complex<double>> twiddles;
    for (size_t length = 2; length <= n; length <<= 1) {
        double angle = 2 * Pi / length;
        std::complex<double> step(std::cos(angle), std::sin(angle));
        std::complex<double> w(1, 0);
        for (size_t j = 0; j < length / 2; ++j) {
            twiddles.push_back(w);
            w *= step;
        }
    }
    return twiddles;
}

void InverseFFT(std::vector<std::complex<double>> &a, const std::vector<std::complex<double>> &twiddles) {
    size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t length = 2; length <= n; length <<= 1) {
        const std::complex<double> *w = twiddles.data() + length / 2 - 1;
        for (size_t start = 0; start < n; start += length) {
            for (size_t j = 0; j < length / 2; ++j) {
                auto even = a[start + j], odd = a[start + j + length / 2] * w[j];
                a[start + j] = even + odd; a[start + j + length / 2] = even - odd;
            }
        }
    }
}

struct AudioState {
    unsigned channels = 0, codedChannels = 0, length = 0, overlap = 0;
    bool dct = false, first = true;
    float root = 0;
    std::vector<unsigned> bands;
    std::vector<float> previous[2];
    std::vector<std::complex<double>> transform;
    // Made once in Init: the FFT factors, the DCT phase factors, and the
    // buffers of a block.
    std::vector<std::complex<double>> twiddles, dctTwiddles;
    std::vector<float> quant, samples[2];
    void Init(unsigned rate, unsigned count, bool useDct) {
        channels = count; dct = useDct; first = true;
        length = rate < 22050 ? 512 : (rate < 44100 ? 1024 : 2048);
        codedChannels = dct ? channels : 1;
        unsigned effectiveRate = rate;
        if (!dct) { length *= channels; effectiveRate *= channels; }
        overlap = length / 16;
        root = (dct ? float(length) : 2.0f) / (std::sqrt(float(length)) * 32768.0f);
        static const unsigned frequencies[] = {
            100, 200, 300, 400, 510, 630, 770, 920, 1080, 1270, 1480, 1720,
            2000, 2320, 2700, 3150, 3700, 4400, 5300, 6400, 7700, 9500, 12000, 15500, 24500
        };
        unsigned half = (effectiveRate + 1) / 2, countBands = 1;
        while (countBands < 25 && half > frequencies[countBands - 1]) ++countBands;
        bands.resize(countBands + 1); bands[0] = 2;
        for (unsigned i = 1; i < countBands; ++i) bands[i] = (frequencies[i - 1] * length / half) & ~1u;
        bands[countBands] = length;
        for (unsigned i = 0; i < codedChannels; ++i) previous[i].assign(overlap, 0);
        transform.resize(dct ? length * 2 : length);
        twiddles = FFTTwiddles(transform.size());
        dctTwiddles.clear();
        for (unsigned k = 0; dct && k < length; ++k) {
            double angle = Pi * k / (2 * length);
            dctTwiddles.emplace_back(std::cos(angle), std::sin(angle));
        }
        quant.resize(bands.size() - 1);
        for (std::vector<float> &s : samples) s.resize(length);
    }
    float Float(Bits &bits) {
        int exponent = bits.Read(5);
        float value = std::ldexp(float(bits.Read(23)), exponent - 23);
        return bits.Read(1) ? -value : value;
    }
    // One block of one channel into c (length samples).
    void Coefficients(Bits &bits, std::vector<float> &c) {
        std::fill(c.begin(), c.end(), 0.0f);
        c[0] = Float(bits) * root; c[1] = Float(bits) * root;
        for (float &q : quant) q = std::exp(std::min(bits.Read(8), 95u) * 0.15289164787221953823f) * root;
        unsigned band = 0, i = 2;
        float q = quant[0];
        static const unsigned runs[] = {2, 3, 4, 5, 6, 8, 9, 10, 11, 12, 13, 14, 15, 16, 32, 64};
        while (i < length) {
            unsigned run = bits.Read(1) ? runs[bits.Read(4)] : 1;
            unsigned end = std::min(i + run * 8, length), width = bits.Read(4);
            if (!width) {
                i = end;
                while (bands[band] < i) q = quant[band++];
            } else {
                while (i < end) {
                    if (bands[band] == i) q = quant[band++];
                    unsigned coefficient = bits.Read(width);
                    c[i++] = coefficient ? (bits.Read(1) ? -q : q) * coefficient : 0;
                }
            }
        }
        std::fill(transform.begin(), transform.end(), std::complex<double>(0, 0));
        if (dct) {
            // DCT III via the even, phase-shifted spectrum of a 2N-point FFT.
            transform[0] = 2 * c[0];
            for (unsigned k = 1; k < length; ++k) {
                transform[k] = double(c[k]) * dctTwiddles[k];
                transform[2 * length - k] = std::conj(transform[k]);
            }
        } else {
            transform[0] = c[0]; transform[length / 2] = c[1];
            for (unsigned k = 1; k < length / 2; ++k) {
                transform[k] = {c[2 * k], -c[2 * k + 1]};
                transform[length - k] = std::conj(transform[k]);
            }
        }
        InverseFFT(transform, twiddles);
        float scale = dct ? 1.0f / length : 0.5f;
        for (unsigned i = 0; i < length; ++i) c[i] = transform[i].real() * scale;
    }
    void Decode(const uint8_t *data, size_t bytes, std::vector<float> &pcm) {
        Require(bytes >= 4, "truncated Bink audio packet");
        unsigned reported = LE32(data);
        Require(reported <= 16 * 1024 * 1024 && reported % (2 * channels) == 0,
                "invalid Bink audio output size");
        Bits bits(data + 4, bytes - 4);
        pcm.clear();
        pcm.reserve(reported / 2 + length * codedChannels);
        while (bits.Left()) {
            Require(pcm.size() < 8 * 1024 * 1024, "Bink audio packet too large");
            if (dct) bits.Read(2);
            for (unsigned ch = 0; ch < codedChannels; ++ch) {
                Coefficients(bits, samples[ch]);
                if (!first) {
                    unsigned total = overlap * codedChannels;
                    for (unsigned i = 0; i < overlap; ++i) {
                        unsigned weight = i * codedChannels + ch;
                        samples[ch][i] = (previous[ch][i] * (total - weight) + samples[ch][i] * weight) / total;
                    }
                }
                std::copy(samples[ch].end() - overlap, samples[ch].end(), previous[ch].begin());
            }
            first = false;
            bits.Align();
            for (unsigned i = 0; i < length - overlap; ++i)
                for (unsigned ch = 0; ch < codedChannels; ++ch) {
                    Require(std::isfinite(samples[ch][i]), "invalid Bink audio sample");
                    pcm.push_back(samples[ch][i]);
                }
        }
        // Last packets can carry fewer valid samples than a complete transform block.
        Require(pcm.size() >= reported / 2, "Bink audio output shorter than reported");
        pcm.resize(reported / 2);
    }
};
} // namespace

struct Decoder::Impl {
    Reader reader = {};
    Info info;
    std::string error;
    VideoState video;
    VideoState chroma;      // the bundles of the chroma worker
    AudioState audio;
    std::vector<std::unique_ptr<Worker>> workers;   // [0] audio, [1] chroma and half the picture
    Timing timing;
    std::vector<uint32_t> offsets;
    unsigned tracks = 0, frame = 0;
    int selected = -1;
    bool ready = false;
    std::vector<uint8_t> packet;

    void Read(uint32_t offset, void *dst, uint32_t bytes) {
        Require(offset <= reader.size && bytes <= reader.size - offset &&
                reader.readAt(reader.context, offset, dst, bytes), "truncated Bink file");
    }
    void Open(Reader source, int track) {
        reader = source;
        Require(reader.readAt && reader.size >= 44, "truncated Bink header");
        uint8_t header[44]; Read(0, header, 44);
        Require(std::memcmp(header, "BIKi", 4) == 0, "unsupported Bink revision (expected BIKi)");
        uint64_t fileSize = uint64_t(LE32(header + 4)) + 8;
        info.frames = LE32(header + 8);
        uint32_t largest = LE32(header + 12);
        info.width = LE32(header + 20); info.height = LE32(header + 24);
        info.fpsNumerator = LE32(header + 28); info.fpsDenominator = LE32(header + 32);
        uint32_t flags = LE32(header + 36);
        tracks = LE32(header + 40);
        Require(fileSize <= reader.size && fileSize >= 44 && largest <= fileSize &&
                largest <= 64 * 1024 * 1024, "invalid Bink file size");
        Require(info.frames && info.frames <= 1000000 && tracks <= 256, "invalid Bink frame/track count");
        Require(info.width && info.height && info.width <= 4096 && info.height <= 4096 &&
                uint64_t(info.width) * info.height <= 4096 * 2160, "invalid Bink dimensions");
        Require(info.fpsNumerator && info.fpsDenominator &&
                uint64_t(info.fpsNumerator) <= uint64_t(info.fpsDenominator) * 1000 &&
                uint64_t(info.fpsDenominator) <= uint64_t(info.fpsNumerator) * 10 &&
                uint64_t(info.frames) * 1000 * info.fpsDenominator / info.fpsNumerator <= UINT32_MAX,
                "invalid Bink frame rate/duration");
        Require((flags & ~0x00100000u) == 0, "unsupported Bink video flags");
        uint64_t tableEnd = 44ull + tracks * 12ull + info.frames * 4ull;
        Require(tableEnd <= fileSize, "truncated Bink frame index");
        std::vector<uint8_t> table(size_t(tableEnd) - 44);
        Read(44, table.data(), uint32_t(table.size()));
        selected = track < 0 || !tracks ? -1 : 0;
        for (unsigned i = 0; i < tracks; ++i)
            if (track >= 0 && LE32(table.data() + tracks * 8 + i * 4) == uint32_t(track)) selected = i;
        if (selected >= 0) {
            uint32_t format = LE32(table.data() + tracks * 4 + selected * 4);
            info.sampleRate = format & 65535;
            info.channels = format & 0x20000000 ? 2 : 1;
            Require(info.sampleRate >= 1000 && info.sampleRate <= 48000, "invalid Bink sample rate");
            audio.Init(info.sampleRate, info.channels, format & 0x10000000);
        }
        offsets.resize(info.frames + 1);
        for (unsigned i = 0; i < info.frames; ++i) offsets[i] = LE32(table.data() + tracks * 12 + i * 4) & ~1u;
        offsets.back() = uint32_t(fileSize);
        Require(offsets.front() >= tableEnd, "Bink frame overlaps header/index");
        for (unsigned i = 0; i < info.frames; ++i)
            Require(offsets[i + 1] > offsets[i] && offsets[i + 1] - offsets[i] <= largest,
                    "invalid Bink frame offset/size");
        video.Init(info.width, info.height, flags & 0x00100000);
        chroma.Init(info.width, info.height, flags & 0x00100000, false);
        ready = true;
    }
    void Decode(Frame &output) {
        Require(ready && frame < info.frames, "end of Bink movie");
        packet.resize(offsets[frame + 1] - offsets[frame]);
        Read(offsets[frame], packet.data(), uint32_t(packet.size()));
        size_t cursor = 0;
        const uint8_t *audioData = nullptr;
        size_t audioBytes = 0;
        output.audio.clear();
        for (unsigned i = 0; i < tracks; ++i) {
            Require(packet.size() - cursor >= 4, "truncated Bink audio length");
            uint32_t bytes = LE32(packet.data() + cursor); cursor += 4;
            Require(bytes <= packet.size() - cursor && (bytes == 0 || bytes >= 4), "invalid Bink audio length");
            if (int(i) == selected && bytes) {
                audioData = packet.data() + cursor;
                audioBytes = bytes;
            }
            cursor += bytes;
        }
        const uint8_t *videoData = packet.data() + cursor;
        const size_t videoBytes = packet.size() - cursor;
        Worker *audioWorker = workers.empty() ? nullptr : workers[0].get();
        Worker *chromaWorker = workers.size() > 1 ? workers[1].get() : nullptr;
        timing = Timing();
        auto decodeAudio = [&] {
            const double start = NowMs();
            audio.Decode(audioData, audioBytes, output.audio);
            timing.audio = NowMs() - start;
        };
        if (audioData && !audioWorker)
            decodeAudio();
        if (audioData && audioWorker)
            audioWorker->Start(decodeAudio);
        try {
            DecodeVideo(video, chroma, chromaWorker, videoData, videoBytes, output, timing);
        } catch (...) {
            if (audioWorker) audioWorker->WaitQuietly();
            throw;
        }
        if (audioWorker) audioWorker->Wait();
        ++frame;
    }
};

Decoder::Decoder() : impl(new Impl) {}
Decoder::~Decoder() = default;
unsigned Decoder::SetWorkers(unsigned count) {
    count = std::min(count, 2u);
    while (impl->workers.size() > count) impl->workers.pop_back();
    try {
        while (impl->workers.size() < count) {
            std::unique_ptr<Worker> worker(new Worker);
            impl->workers.push_back(std::move(worker));
        }
    } catch (const std::exception &) {
        // No thread (the Vita fails when its memory budget is spent): fewer workers.
    }
    return unsigned(impl->workers.size());
}
bool Decoder::Open(Reader reader, int track) {
    std::vector<std::unique_ptr<Worker>> workers = std::move(impl->workers);
    impl.reset(new Impl);
    impl->workers = std::move(workers);
    try { impl->Open(reader, track); return true; }
    catch (const std::exception &e) { impl->error = e.what(); impl->ready = false; return false; }
}
bool Decoder::Decode(Frame &frame) {
    try { impl->Decode(frame); return true; }
    catch (const std::exception &e) {
        impl->error = e.what(); impl->ready = false;
        frame.audio.clear(); frame.bgrx.clear();
        return false;
    }
}
const Info &Decoder::GetInfo() const { return impl->info; }
const Timing &Decoder::GetTiming() const { return impl->timing; }
const std::string &Decoder::Error() const { return impl->error; }
} // namespace Bink
