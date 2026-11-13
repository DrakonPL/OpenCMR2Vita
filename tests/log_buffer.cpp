// The buffer of the log lines between two writes to the file
// (src/platform/log_buffer.h): the lines come out in their order, each once,
// while new lines go into the other buffer; a line with no room is lost and
// counted, and nothing is written past the buffer.

#include "platform/log_buffer.h"

#include <cstdio>
#include <string>

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "failed line %d: %s\n", __LINE__, #x); return 1; } } while (0)

int main()
{
    static LogBuffer<32> log;
    size_t bytes = 99;

    // Nothing yet.
    log.Take(&bytes);
    REQUIRE(bytes == 0 && log.Used() == 0);

    // Lines come out with their newlines, in their order.
    REQUIRE(log.Append("one") && log.Append("two") && log.Used() == 8);
    const char *first = log.Take(&bytes);
    REQUIRE(std::string(first, bytes) == "one\ntwo\n" && log.Used() == 0);

    // The lines after a Take go into the other buffer: those that were
    // taken stay as they are while the file gets them.
    REQUIRE(log.Append("three"));
    REQUIRE(std::string(first, 8) == "one\ntwo\n");
    const char *second = log.Take(&bytes);
    REQUIRE(second != first && std::string(second, bytes) == "three\n");
    // Each line comes out once.
    log.Take(&bytes);
    REQUIRE(bytes == 0);

    // A buffer of 32 bytes holds a line of 31 with its newline, and no more.
    const std::string full(31, 'x');
    REQUIRE(log.Append(full.c_str()) && log.Used() == 32);
    REQUIRE(!log.Append("") && !log.Append("more") && log.Used() == 32);
    REQUIRE(log.TakeLost() == 2 && log.TakeLost() == 0);
    const char *third = log.Take(&bytes);
    REQUIRE(bytes == 32 && std::string(third, bytes) == full + "\n");
    // A line too long for an empty buffer is lost, and the next one fits.
    REQUIRE(!log.Append(std::string(32, 'y').c_str()) && log.Used() == 0 && log.Append("ok"));
    REQUIRE(log.TakeLost() == 1);
    const char *fourth = log.Take(&bytes);
    REQUIRE(std::string(fourth, bytes) == "ok\n");

    printf("log_buffer: ok\n");
    return 0;
}
