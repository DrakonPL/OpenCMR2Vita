// The read-ahead of a streamed file (src/platform/read_ahead.h): the reader
// gets the bytes of the file as plain reads give them, in parts of any size,
// after seeks, and short at the end of the file. The block after the one the
// reader takes is read while it takes it, and one read at a time is in
// flight. An error ends a read short.

#include "platform/read_ahead.h"

#include <cstdio>
#include <cstring>
#include <vector>

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "failed line %d: %s\n", __LINE__, #x); return 1; } } while (0)

namespace {

// A file in memory. A read starts in Start and ends in Wait; a Start while a
// read is in flight is noted.
struct MemoryIo : ReadAheadIo {
    std::vector<uint8_t> file;
    bool pending = false;
    int overlapping = 0, starts = 0;
    int result = 0;
    uint64_t failAt = ~0ull;    // a read of this offset fails

    bool Start(uint64_t offset, void *buffer, uint32_t size) override
    {
        overlapping += pending ? 1 : 0;
        starts++;
        pending = true;
        if (offset == failAt) {
            result = -1;
            return true;
        }
        const uint64_t end = offset + size < file.size() ? offset + size : file.size();
        result = offset < file.size() ? (int)(end - offset) : 0;
        if (result > 0)
            memcpy(buffer, file.data() + offset, (size_t)result);
        return true;
    }

    int Wait() override
    {
        pending = false;
        return result;
    }
};

} // namespace

int main()
{
    MemoryIo io;
    io.file.resize(100000);
    for (size_t i = 0; i < io.file.size(); i++)
        io.file[i] = (uint8_t)(i * 7 + i / 251);
    ReadAhead ahead(0x8000);

    // The file from its start in parts of 16 KB, as the music reads it: each
    // part as in the file. After the first read the next block is in flight.
    std::vector<uint8_t> part(0x4000);
    uint64_t at = 0;
    REQUIRE(ahead.Read(io, part.data(), 0x4000) == 0x4000);
    REQUIRE(memcmp(part.data(), io.file.data(), 0x4000) == 0 && io.pending && io.starts == 2);
    at = 0x4000;
    for (;;) {
        const uint32_t n = ahead.Read(io, part.data(), 0x4000);
        REQUIRE(memcmp(part.data(), io.file.data() + at, n) == 0);
        at += n;
        if (n < 0x4000)
            break;
    }
    REQUIRE(at == io.file.size() && ahead.Position() == io.file.size());
    // Four blocks of 32 KB (the last one short): no block read twice.
    REQUIRE(io.starts == 4 && io.overlapping == 0);
    REQUIRE(ahead.Read(io, part.data(), 16) == 0);

    // A seek back to the data (the music loops) and parts of odd sizes.
    ahead.Seek(io, 44);
    at = 44;
    const uint32_t sizes[] = { 1, 999, 0x4000, 32767, 3, 0x8000 + 5 };
    for (uint32_t size : sizes) {
        std::vector<uint8_t> out(size);
        const uint32_t n = ahead.Read(io, out.data(), size);
        REQUIRE(n == size && memcmp(out.data(), io.file.data() + at, n) == 0);
        at += n;
    }
    REQUIRE(io.overlapping == 0);

    // A seek with a read in flight ends it first.
    REQUIRE(io.pending);
    ahead.Seek(io, 99990);
    REQUIRE(!io.pending);
    std::vector<uint8_t> tail(64);
    REQUIRE(ahead.Read(io, tail.data(), 64) == 10 && memcmp(tail.data(), io.file.data() + 99990, 10) == 0);

    // A read that fails ends the reader's read short; a later read tries again.
    ReadAhead failing(0x8000);
    MemoryIo bad;
    bad.file = io.file;
    bad.failAt = 0x8000;
    std::vector<uint8_t> big(0x10000);
    REQUIRE(failing.Read(bad, big.data(), 0x10000) == 0x8000);
    bad.failAt = ~0ull;
    REQUIRE(failing.Read(bad, big.data() + 0x8000, 0x8000) == 0x8000);
    REQUIRE(memcmp(big.data(), bad.file.data(), 0x10000) == 0 && bad.overlapping == 0);
    failing.Close(bad);
    ahead.Close(io);
    REQUIRE(!io.pending && !bad.pending);

    printf("read_ahead: ok\n");
    return 0;
}
