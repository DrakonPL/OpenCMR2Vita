// The default settings of the PS Vita (src/platform/vita_settings.cpp): the
// values the table gives, the first file a run writes, and the language map.

#include "platform/sys_common.h"
#include "platform/vita_settings.h"
#include "port/sys.h"

#include <cstdio>
#include <string>

// sys_common.cpp's step timer; no test here reaches it.
extern "C" DWORD Sys_GetTicks(void) { return 0; }

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "failed line %d: %s\n", __LINE__, #x); return 1; } } while (0)

namespace {

int TestDefaults()
{
    SysSettings::Values values;
    SysSettings::Parse(VitaSettings::kDefaults, values);
    REQUIRE(values["video.outline_fonts"] == "0");
    REQUIRE(values["video.vector_icons"] == "0");
    REQUIRE(values["video.car_detail"] == "0");
    REQUIRE(values["video.hud_aspect"] == "16:9");
    REQUIRE(values["input.key_repeat_delay"] == "500");
    REQUIRE(values["input.key_repeat_interval"] == "33");
    // The system gives the language when the file does not.
    REQUIRE(values.count("install.language") == 0);
    return 0;
}

int TestFirstFile()
{
    const std::string file = VitaSettings::CommentedOut(VitaSettings::kDefaults);
    SysSettings::Values values;
    SysSettings::Parse(file, values);
    REQUIRE(values.empty());
    REQUIRE(file.find("[video]\n") != std::string::npos);
    REQUIRE(file.find("\n; car_detail = 0\n") != std::string::npos);
    REQUIRE(file.find("\n; ; ") == std::string::npos);

    // A user edit wins over the table.
    SysSettings::Parse(VitaSettings::kDefaults, values);
    SysSettings::Parse(SysSettings::Edit(file, "video", "car_detail", "1"), values);
    REQUIRE(values["video.car_detail"] == "1");
    REQUIRE(values["video.outline_fonts"] == "0");
    return 0;
}

int TestLanguage()
{
    // SCE_SYSTEM_PARAM_LANG_*: 1 English (US), 2 French, 3 Spanish, 4 German,
    // 5 Italian, 16 Polish, 18 English (GB).
    REQUIRE(std::string(VitaSettings::Language(2)) == "French");
    REQUIRE(std::string(VitaSettings::Language(3)) == "Spanish");
    REQUIRE(std::string(VitaSettings::Language(4)) == "German");
    REQUIRE(std::string(VitaSettings::Language(5)) == "Italian");
    REQUIRE(std::string(VitaSettings::Language(1)) == "English");
    REQUIRE(std::string(VitaSettings::Language(18)) == "English");
    REQUIRE(std::string(VitaSettings::Language(16)) == "English");
    REQUIRE(std::string(VitaSettings::Language(-1)) == "English");
    return 0;
}

} // namespace

int main()
{
    if (TestDefaults() || TestFirstFile() || TestLanguage())
        return 1;
    printf("vita_settings: ok\n");
    return 0;
}
