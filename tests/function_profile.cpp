// The function profile (src/diagnostics/function_profile.cpp) of a profiling
// build: it times the game's thread only. Game code that runs on another
// thread (the music refills) goes into no entry and moves no depth, else a
// hook read an entry that the other thread had changed (a crash on the PS Vita).

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "failed line %d: %s\n", __LINE__, #x); return 1; } } while (0)

extern "C" void __cyg_profile_func_enter(void *function, void *caller);
extern "C" void __cyg_profile_func_exit(void *function, void *caller);

namespace {

std::vector<std::string> s_lines;

void GameFunction() {}
void MusicFunction() {}

} // namespace

// The frame function, by its linker name (the profile does not see the game).
extern "C" void _ZN5CGame29DispatchFrontendResourceStateEv() {}

extern "C" void Sys_Log(const char *format, ...)
{
    char line[512];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    s_lines.push_back(line);
}

int main()
{
    void *const frame = (void *)&_ZN5CGame29DispatchFrontendResourceStateEv;
    void *const game = (void *)&GameFunction;
    void *const music = (void *)&MusicFunction;
    char address[32];

    // 300 frames (one report). In each, the other thread calls a function
    // while the game's thread is inside one.
    for (int i = 0; i < 300; i++) {
        __cyg_profile_func_enter(frame, nullptr);
        __cyg_profile_func_enter(game, nullptr);
        std::thread other([&] {
            __cyg_profile_func_enter(music, nullptr);
            __cyg_profile_func_enter(music, nullptr);
            __cyg_profile_func_exit(music, nullptr);
        });
        other.join();
        __cyg_profile_func_exit(game, nullptr);
        __cyg_profile_func_exit(frame, nullptr);
    }

    // The report: the game's function at depth 0, once in each frame; the
    // other thread's function is not in it.
    REQUIRE(!s_lines.empty() && strstr(s_lines[0].c_str(), "300 frames") != nullptr);
    snprintf(address, sizeof(address), "%p depth 0:", game);
    bool gameFound = false;
    for (const std::string &line : s_lines) {
        gameFound = gameFound || (strstr(line.c_str(), address) != nullptr &&
                                  strstr(line.c_str(), "1.0 calls for each frame") != nullptr);
        snprintf(address, sizeof(address), "%p ", music);
        REQUIRE(strstr(line.c_str(), address) == nullptr);
        snprintf(address, sizeof(address), "%p depth 0:", game);
    }
    REQUIRE(gameFound);
    printf("function_profile: ok\n");
    return 0;
}
