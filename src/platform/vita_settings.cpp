#include "platform/vita_settings.h"

#include <sstream>

namespace VitaSettings {

const char kDefaults[] =
    "; OpenCMR2 settings (PS Vita)\n"
    "; Remove the ';' in front of a setting to change it.\n"
    "\n"
    "[install]\n"
    "; English, French, German, Italian, Spanish, American or Polish. Without a\n"
    "; language line: the language of the system, or English.\n"
    "; EUROPE, AMERICA, POLAND or JAPAN. The game data decides when it has one only.\n"
    "sku = EUROPE\n"
    "\n"
    "[video]\n"
    "; Size of the in-race HUD and of the menus, in percent of the original's at\n"
    "; the screen's height. 0 = the original's layout in pixels.\n"
    "hud_scale = 100\n"
    "menu_scale = 100\n"
    "; Widest shape the HUD spreads over: an aspect ratio (16:9) or full.\n"
    "hud_aspect = 16:9\n"
    "; 1 = 60 frames a second at most, 2 = 30, 0 = no wait for the display.\n"
    "vsync = 1\n"
    "; 1 = stretch the movies over the screen, 0 = keep their proportions.\n"
    "stretch_movies = 0\n"
    "; 1 = every car with the player's model and textures, 0 = the original's\n"
    "; lower detail for the other cars.\n"
    "car_detail = 0\n"
    "; 1 = text drawn from outlines. The fit of the outlines takes about 20 s at\n"
    "; each start. 0 = the original's bitmap fonts.\n"
    "outline_fonts = 0\n"
    "; 1 = icons traced as outlines. The trace takes about 48 s at the first\n"
    "; stage load. 0 = the original's bitmaps.\n"
    "vector_icons = 0\n"
    "; 1 = the sky at half the size, stretched (faster in rain and fog), 0 = full.\n"
    "sky_half = 1\n"
    "; Threads of the Bink movie decoder, 0 to 2.\n"
    "movie_workers = 2\n"
    "; Vertices relit in a frame when the light of the scene changes, 0 = all.\n"
    "relight_budget = 10000\n"
    "; 1 = parts of the game's frame (the tyre marks, the sector cull) run on\n"
    "; core 1, 0 = on the core of the game. 2 = both, and the log names each\n"
    "; difference.\n"
    "game_jobs = 1\n"
    "\n"
    "[input]\n"
    "; Milliseconds before a button held in the menus repeats, and between repeats.\n"
    "key_repeat_delay = 500\n"
    "key_repeat_interval = 33\n"
    "; The bindings of the buttons are in controls.ini.\n"
    "\n"
    "[net]\n"
    "; 1 = races over the network (LAN). 0 loads no network module.\n"
    "enabled = 0\n";

std::string CommentedOut(const std::string &text)
{
    std::istringstream in(text);
    std::string out, line;
    while (std::getline(in, line)) {
        const size_t first = line.find_first_not_of(" \t");
        const bool setting = first != std::string::npos && line[first] != ';' && line[first] != '[' &&
                             line.find('=') != std::string::npos;
        out += (setting ? "; " : "") + line + "\n";
    }
    return out;
}

const char *Language(int systemLanguage)
{
    // The numbers of SCE_SYSTEM_PARAM_LANG_* in psp2/system_param.h.
    static const struct { int system; const char *game; } kLanguages[] = {
        { 2, "French" }, { 3, "Spanish" }, { 4, "German" }, { 5, "Italian" },
    };
    for (const auto &language : kLanguages)
        if (language.system == systemLanguage)
            return language.game;
    return "English";
}

} // namespace VitaSettings
