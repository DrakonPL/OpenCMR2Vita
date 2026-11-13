// The lines of the log between two writes to the file. A write to the
// memory card of the PS Vita takes some 13 ms: the thread that logs only
// copies its line here, and a thread of its own writes the lines to the file
// (sys_vita.cpp). Two buffers: the lines go into one while the other is
// written. No lock here: the caller holds one for Append and Take.
// tests/log_buffer.cpp.
#ifndef OPENCMR2_LOG_BUFFER_H
#define OPENCMR2_LOG_BUFFER_H

#include <stddef.h>
#include <string.h>

template <size_t kBytes> struct LogBuffer {
    // Adds the line and a newline. With no room for it the line is lost and
    // counted: the caller must not wait for the card.
    bool Append(const char *line)
    {
        const size_t length = strlen(line);
        if (used + length + 1 > kBytes) {
            lost++;
            return false;
        }
        memcpy(buffers[active] + used, line, length);
        used += length;
        buffers[active][used++] = '\n';
        return true;
    }

    // The lines since the last Take, for the file: *bytes of them. They stay
    // until the next Take. The lines after this call go into the other
    // buffer.
    const char *Take(size_t *bytes)
    {
        const char *taken = buffers[active];
        *bytes = used;
        active ^= 1;
        used = 0;
        return taken;
    }

    // The lines that found no room since the last call.
    unsigned TakeLost()
    {
        const unsigned count = lost;
        lost = 0;
        return count;
    }

    size_t Used() const { return used; }

private:
    char buffers[2][kBytes];
    int active = 0;
    size_t used = 0;
    unsigned lost = 0;
};

#endif
