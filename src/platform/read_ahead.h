// A read-ahead over a file that is read from its start to its end in parts
// (the streamed music): two blocks of the file in memory. While the reader
// takes its bytes from one, the next one is read asynchronously, so a read
// waits for the card only where the reader is faster than the card. A seek
// keeps the blocks: the file does not change while it is open. No platform
// calls: the file goes through ReadAheadIo (sys_vita.cpp: sceIoReadAsync),
// and tests/read_ahead.cpp runs it on the PC.
#ifndef OPENCMR2_READ_AHEAD_H
#define OPENCMR2_READ_AHEAD_H

#include <stdint.h>
#include <string.h>

#include <utility>
#include <vector>

// The file under the read-ahead. One read at a time: Start begins a read of
// size bytes at offset, Wait returns its byte count (below size at the end of
// the file) or a negative number for an error.
struct ReadAheadIo {
    virtual ~ReadAheadIo() {}
    virtual bool Start(uint64_t offset, void *buffer, uint32_t size) = 0;
    virtual int Wait() = 0;
};

class ReadAhead {
public:
    explicit ReadAhead(uint32_t blockBytes) : blockBytes(blockBytes)
    {
        current.data.resize(blockBytes);
        next.data.resize(blockBytes);
    }

    uint64_t Position() const { return position; }

    // Sets the position of the next Read. A read in flight ends first.
    void Seek(ReadAheadIo &io, uint64_t offset)
    {
        Finish(io);
        position = offset;
    }

    // Ends a read in flight (before the file is used another way).
    void Finish(ReadAheadIo &io)
    {
        if (!next.pending)
            return;
        const int n = io.Wait();
        next.pending = false;
        next.valid = n >= 0;
        next.length = n >= 0 ? (uint32_t)n : 0;
    }

    // Copies up to size bytes from the position into out and returns how
    // many: fewer at the end of the file or after an error.
    uint32_t Read(ReadAheadIo &io, void *out, uint32_t size)
    {
        uint32_t done = 0;
        while (done < size) {
            if (Covers(current)) {
                const uint32_t at = (uint32_t)(position - current.offset);
                uint32_t n = current.length - at;
                n = n < size - done ? n : size - done;
                memcpy((uint8_t *)out + done, current.data.data() + at, n);
                done += n;
                position += n;
                continue;
            }
            Finish(io);
            if (Covers(next)) {
                std::swap(current, next);
                Prefetch(io);
                continue;
            }
            // The end of the file: a block that came short, before the position.
            if (AtEnd(current) || AtEnd(next))
                break;
            // Nothing in memory has the position: read its block now.
            if (!io.Start(position, current.data.data(), blockBytes)) {
                current.valid = false;
                break;
            }
            const int n = io.Wait();
            current.offset = position;
            current.valid = n > 0;
            current.length = n > 0 ? (uint32_t)n : 0;
            if (n <= 0)
                break;
            Prefetch(io);
        }
        return done;
    }

    void Close(ReadAheadIo &io) { Finish(io); }

private:
    struct Block {
        std::vector<uint8_t> data;
        uint64_t offset = 0;
        uint32_t length = 0;
        bool valid = false;     // offset and length tell the bytes in data
        bool pending = false;   // a read into it is in flight
    };

    bool Covers(const Block &b) const
    {
        return b.valid && position >= b.offset && position < b.offset + b.length;
    }

    bool AtEnd(const Block &b) const
    {
        return b.valid && b.length < blockBytes && position >= b.offset + b.length &&
               position < b.offset + blockBytes;
    }

    // The block after current, read while the reader takes current. None
    // after the last block of the file.
    void Prefetch(ReadAheadIo &io)
    {
        next.valid = false;
        if (current.length < blockBytes)
            return;
        next.offset = current.offset + current.length;
        next.pending = io.Start(next.offset, next.data.data(), blockBytes);
    }

    uint32_t blockBytes;
    uint64_t position = 0;
    Block current, next;
};

#endif
