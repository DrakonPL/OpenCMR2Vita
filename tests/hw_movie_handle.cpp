// The value of sceAvPlayerInit (video/hw_movie_handle.h): a player is an
// address, below zero as an int. An error code is 0x80xxxxxx.

#include "video/hw_movie_handle.h"

#include <cstdio>

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "failed line %d: %s\n", __LINE__, #x); return 1; } } while (0)

int main()
{
    // The handles of two movies on the device (the log of the first run).
    REQUIRE(HwMovie_HandleIsPlayer((int)0x82c47d00u));
    REQUIRE(HwMovie_HandleIsPlayer((int)0x825e7c00u));
    // An address in the first memory of a program, and a small handle.
    REQUIRE(HwMovie_HandleIsPlayer((int)0x81000000u));
    REQUIRE(HwMovie_HandleIsPlayer(1));
    // Nothing, and the error codes of the player (0x806a0001 and up).
    REQUIRE(!HwMovie_HandleIsPlayer(0));
    REQUIRE(!HwMovie_HandleIsPlayer((int)0x806a0001u));
    REQUIRE(!HwMovie_HandleIsPlayer((int)0x806a00a0u));
    REQUIRE(!HwMovie_HandleIsPlayer((int)0x80000000u));
    REQUIRE(!HwMovie_HandleIsPlayer((int)0x80ffffffu));

    printf("ok\n");
    return 0;
}
