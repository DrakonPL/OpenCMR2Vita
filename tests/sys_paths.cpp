// The shared game path resolver (src/platform/sys_paths.cpp) on a fake file
// system: DOS paths, drive letters, case folding and the merged file list.

#include "platform/sys_paths.h"

#include <cstdio>
#include <map>
#include <string>

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "failed line %d: %s\n", __LINE__, #x); return 1; } } while (0)

namespace {

// Host paths -> kind; a directory's children are the paths one level below it.
struct FakeHost : SysPaths::Host {
    std::map<std::string, SysPaths::Kind> paths;
    int listings = 0;
    std::string made;

    void List(const std::string &dir, SysPaths::Entries &entries) override
    {
        listings++;
        for (const auto &p : paths) {
            if (p.first.compare(0, dir.size(), dir) != 0 || p.first.size() == dir.size())
                continue;
            const std::string rest = p.first.substr(dir.size());
            if (rest.find('/') == std::string::npos)
                entries.emplace(SysPaths::Lower(rest), rest);
        }
    }
    void MakeDirectory(const std::string &path) override
    {
        made = path;
        paths[path] = SysPaths::Kind::Directory;
    }
    SysPaths::Kind GetKind(const std::string &path, uint64_t *size) override
    {
        auto it = paths.find(path);
        if (it == paths.end())
            return SysPaths::Kind::None;
        if (size != nullptr)
            *size = 100;
        return it->second;
    }
};

} // namespace

int main()
{
    using SysPaths::Kind;
    FakeHost host;
    host.paths = {
        {"data/FrontEnd", Kind::Directory},
        {"data/FrontEnd/MENU.TGA", Kind::File},
        {"data/Error.TXT", Kind::File},
        {"data/SaveGame", Kind::Directory},
        {"data/SaveGame/Slot1.SAV", Kind::File},
        {"data/SaveGame/old.sav", Kind::File},
        {"data/SaveGame/notes.txt", Kind::File},
        {"user/savegame", Kind::Directory},
        {"user/savegame/slot1.sav", Kind::File},
        {"user/savegame/new.Sav", Kind::File},
    };
    SysPaths::Cache cache(host);

    REQUIRE(cache.ResolveForReading("user/", "data/", "Frontend\\menu.tga") == "data/FrontEnd/MENU.TGA");
    REQUIRE(cache.ResolveForReading("user/", "data/", "c:\\error.txt") == "data/Error.TXT");
    REQUIRE(cache.ResolveForReading("user/", "data/", "SAVEGAME\\SLOT1.SAV") == "user/savegame/slot1.sav");
    REQUIRE(cache.ResolveForReading("user/", "data/", ".\\FrontEnd\\..\\frontend\\Menu.tga") == "data/FrontEnd/MENU.TGA");
    REQUIRE(cache.ResolveForReading("user/", "data/", "Frontend\\missing.tga").empty());
    REQUIRE(cache.ResolveForReading("user/", "data/", "c:\\").empty());

    // The user directory's copy wins and the names keep their case on disk.
    const std::vector<std::string> saves = cache.ListFiles("data/", "user/", "SaveGame", "*.sav");
    REQUIRE(saves.size() == 3);
    REQUIRE(saves[0] == "new.Sav" && saves[1] == "old.sav" && saves[2] == "slot1.sav");

    // Each directory is listed once until the cache is cleared.
    const int listings = host.listings;
    REQUIRE(cache.ResolveForReading("user/", "data/", "frontend\\MENU.tga") == "data/FrontEnd/MENU.TGA");
    REQUIRE(host.listings == listings);
    cache.Clear();
    REQUIRE(cache.ResolveForReading("user/", "data/", "frontend\\MENU.tga") == "data/FrontEnd/MENU.TGA");
    REQUIRE(host.listings > listings);

    // Writing reuses an existing directory whatever its case and makes a missing one.
    REQUIRE(cache.PathForWriting("user/", SysPaths::SplitGamePath("SaveGame\\x.sav")) == "user/savegame/x.sav");
    REQUIRE(host.made.empty());
    REQUIRE(cache.PathForWriting("user/", SysPaths::SplitGamePath("Replays\\r.rpl")) == "user/Replays/r.rpl");
    REQUIRE(host.made == "user/Replays");

    REQUIRE(SysPaths::MatchPattern("SLOT1.SAV", "*.sav") && !SysPaths::MatchPattern("slot1.sa", "*.sav"));
    REQUIRE(cache.MissingGameData("data/").find("Game/Bigfiles/common.bfl") != std::string::npos);
    puts("sys_paths: ok");
    return 0;
}
