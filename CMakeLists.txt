cmake_minimum_required(VERSION 3.24)
project(OpenCMR2 LANGUAGES C CXX)
if(VITA)
    enable_language(ASM)   # embeds the precompiled GXM shaders (src/CMakeLists.txt)
endif()

# Targets (see CMakePresets.json):
#  - linux-x86: 32-bit x86, the reference build. The game's data structures
#    have the original's layout there, which the golden tests compare against.
#  - linux, windows-x64, macos: native 64-bit builds. Raw access review and
#    original-game parity checks are tracked in docs/64bit-raw-access.md.
#  - Android: built by the Gradle project in android/, which passes the NDK
#    toolchain and the SDL3 source it also takes SDLActivity from.
#  - vita: PS Vita with the vitasdk GCC (cmake/vita.cmake), without SDL.

# The game source needs Clang with GNU-style options: -fms-extensions for the
# MSVC dialect it was written in, forced includes and the semantics below.
# On Windows that is clang++ (not clang-cl) targeting the MSVC runtime.
# The PS Vita build uses the vitasdk GCC, which takes the same options.
if(VITA AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_C_COMPILER_ID STREQUAL "GNU")
    message(STATUS "OpenCMR2: PS Vita build with GCC ${CMAKE_CXX_COMPILER_VERSION}")
elseif(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "OpenCMR2 builds with Clang (found ${CMAKE_CXX_COMPILER_ID}); use one of the CMake presets")
endif()
if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    message(FATAL_ERROR "Use clang++, not clang-cl: the build passes GNU-style options")
endif()

option(OPENCMR2_SANITIZE "Build with AddressSanitizer and UBSan" OFF)
option(OPENCMR2_DEV "Dev build: probe and heartbeat lines in the log" OFF)
option(OPENCMR2_PROFILE_FUNCTIONS "Profiling build: the time of each function of the game in the log (slow)" OFF)
option(OPENCMR2_VENDORED_SDL "Download and build SDL3 instead of using an installed one" ${ANDROID})

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 99)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Semantics the decompiled code relies on:
#  - signed overflow wraps (16.16 fixed point overflows on purpose);
#  - type punning through casts (the code was written against MSVC6);
#  - plain char is signed, as in MSVC (it is unsigned on ARM Linux);
#  - no fused multiply-add: the original rounds every product, and ARM64
#    compilers would otherwise fuse a * b + c and change physics results;
#  - explicit float stores use single precision, doubles use double precision;
#    the original's x87 nearest-even integer conversions are written explicitly
#    at audited physics sites (ordinary C++ casts would truncate).
add_compile_options(-fwrapv -fno-strict-aliasing -fsigned-char -ffp-contract=off)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86|i[3-6]86|AMD64")
    add_compile_options(-msse2 -mfpmath=sse)
endif()
if(WIN32)
    # The MSVC runtime warns about its own POSIX names and "unsafe" functions.
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS NOMINMAX)
endif()
if(VITA)
    add_compile_definitions(VITA)
    # The vitasdk GCC schedules for a generic ARMv7-A. The PS Vita has four Cortex-A9 cores.
    add_compile_options(-mtune=cortex-a9)
endif()
if(OPENCMR2_DEV)
    add_compile_definitions(OPENCMR2_DEV)
endif()

if(OPENCMR2_SANITIZE)
    # The game calls its callback tables through generic function types, as
    # the original does, so UBSan's function type check is off.
    add_compile_options(-fsanitize=address,undefined -fno-sanitize=function -fno-omit-frame-pointer
                        -fsanitize-ignorelist=${CMAKE_SOURCE_DIR}/tools/sanitizer-ignorelist.txt)
    add_link_options(-fsanitize=address,undefined)
endif()

if(NOT OPENCMR2_VENDORED_SDL AND NOT VITA)
    find_package(SDL3 3.4 CONFIG COMPONENTS SDL3-shared)
endif()
if(NOT TARGET SDL3::SDL3 AND NOT VITA)
    message(STATUS "OpenCMR2: building SDL3 from source")
    set(OPENCMR2_BUILT_SDL ON)
    include(FetchContent)
    set(SDL_SHARED ON CACHE BOOL "" FORCE)
    set(SDL_STATIC OFF CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(SDL3
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG release-3.4.18
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(SDL3)
    # SDL's own warnings, not ours to fix: its EGL typedefs are declared again
    # by the NDK's headers, and the Android sensor code uses a deprecated call.
    if(TARGET SDL3-shared AND CMAKE_C_COMPILER_ID MATCHES "Clang")
        target_compile_options(SDL3-shared PRIVATE -Wno-typedef-redefinition -Wno-deprecated-declarations)
    endif()
endif()

add_subdirectory(src)

if(VITA)
    # The eboot and the .vpk (build/vita/opencmr2.vpk).
    include(${VITASDK}/share/vita.cmake REQUIRED)
    # The vitasdk linker can make a writable and executable segment, which the
    # firmware refuses to start: fix_vita_rwe.py makes it read and execute only.
    set(VITA_VELF ${CMAKE_CURRENT_BINARY_DIR}/opencmr2.velf)
    set(VITA_EBOOT ${CMAKE_CURRENT_BINARY_DIR}/eboot.bin)
    add_custom_command(
        OUTPUT ${VITA_EBOOT}
        COMMAND ${VITA_ELF_CREATE} $<TARGET_FILE:opencmr2> ${VITA_VELF}
        COMMAND python3 ${PROJECT_SOURCE_DIR}/tools/fix_vita_rwe.py ${VITA_VELF}
        COMMAND ${VITA_MAKE_FSELF} -c ${VITA_VELF} ${VITA_EBOOT}
        DEPENDS opencmr2 ${PROJECT_SOURCE_DIR}/tools/fix_vita_rwe.py
        VERBATIM
        COMMENT "Making eboot.bin")

    # res/vita-local (not in git, tools/make_livearea.py) replaces the placeholders.
    set(VITA_SCE_SYS ${PROJECT_SOURCE_DIR}/res/vita/sce_sys)
    if(EXISTS ${PROJECT_SOURCE_DIR}/res/vita-local/sce_sys/icon0.png)
        set(VITA_SCE_SYS ${PROJECT_SOURCE_DIR}/res/vita-local/sce_sys)
    endif()
    message(STATUS "OpenCMR2: LiveArea images from ${VITA_SCE_SYS}")
    vita_create_vpk(opencmr2.vpk OCMR20001 ${VITA_EBOOT}
        VERSION 01.00
        NAME OpenCMR2
        FILE ${VITA_SCE_SYS}/icon0.png sce_sys/icon0.png
        FILE ${VITA_SCE_SYS}/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
        FILE ${VITA_SCE_SYS}/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
        FILE ${VITA_SCE_SYS}/livearea/contents/template.xml sce_sys/livearea/contents/template.xml)
    # VitaShell does not install a .vpk with a name outside ASCII.
    add_custom_command(TARGET opencmr2.vpk-vpk POST_BUILD
        COMMAND python3 ${PROJECT_SOURCE_DIR}/tools/check_vpk_names.py ${CMAKE_CURRENT_BINARY_DIR}/opencmr2.vpk
        VERBATIM)
endif()

# The tests run on the build machine; the Android and Vita builds are the game alone.
if(NOT ANDROID AND NOT VITA)
    enable_testing()
    add_subdirectory(tests)
endif()
