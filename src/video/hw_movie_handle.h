// Whether the value that sceAvPlayerInit gives is a player. The handle is
// the address of the player (0x81000000 and up on the PS Vita): below zero
// as an int. An error code of the system is 0x80xxxxxx, and no memory of a
// program is there. tests/hw_movie_handle.cpp checks it on the PC.
#ifndef OPENCMR2_HW_MOVIE_HANDLE_H
#define OPENCMR2_HW_MOVIE_HANDLE_H

inline bool HwMovie_HandleIsPlayer(int handle)
{
    return handle != 0 && ((unsigned)handle >> 24) != 0x80u;
}

#endif
