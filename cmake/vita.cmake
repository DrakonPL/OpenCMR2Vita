# PS Vita build with the vitasdk GCC (the vita preset, run in the vitasdk Docker image).
include($ENV{VITASDK}/share/vita.toolchain.cmake)
set(VITA ON CACHE BOOL "PS Vita build" FORCE)
