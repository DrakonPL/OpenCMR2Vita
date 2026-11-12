// Game paths on the host file system, shared by sys.cpp (SDL) and sys_vita.cpp.
// Game paths are the original's DOS-style paths ("Frontend\\menu.tga"); names
// are matched without regard to case, each directory listed once.
#ifndef OPENCMR2_SYS_PATHS_H
#define OPENCMR2_SYS_PATHS_H

#include <stdint.h>

#include <map>
#include <string>
#include <vector>

namespace SysPaths {

// Lower-case name -> name on disk.
typedef std::map<std::string, std::string> Entries;

enum class Kind { None, File, Directory };

// The host file system calls that the path code needs.
struct Host {
    virtual ~Host() = default;
    // Adds the direct children of dir (a path that ends with '/').
    virtual void List(const std::string &dir, Entries &entries) = 0;
    virtual void MakeDirectory(const std::string &path) = 0;
    // size may be NULL; it is set for a file.
    virtual Kind GetKind(const std::string &path, uint64_t *size) = 0;
};

// ASCII only, as the game's names are.
std::string Lower(const std::string &s);
// Splits a game path into its components: drive letters, "." and empty
// components dropped, ".." applied.
std::vector<std::string> SplitGamePath(const char *path);
// One '*' at most, compared without regard to case ("*.sav").
bool MatchPattern(const std::string &name, const std::string &pattern);
std::string WithSlash(std::string dir);

class Cache {
public:
    explicit Cache(Host &host) : _host(host) {}

    Host &GetHost() { return _host; }
    const Entries &List(const std::string &dir);
    void Clear() { _dirs.clear(); }

    // The host path of parts under root, or "".
    std::string FindUnder(const std::string &root, const std::vector<std::string> &parts);
    // The host path for writing under userDir; makes the directories and
    // reuses existing ones whatever their case.
    std::string PathForWriting(const std::string &userDir, const std::vector<std::string> &parts);
    // The user directory's copy first, then the game data's; "" if neither.
    std::string ResolveForReading(const std::string &userDir, const std::string &dataDir, const char *gamePath);
    // The required archives and folders missing from dir, one "\n- name" each.
    std::string MissingGameData(const std::string &dir);
    // The files of a game directory that match pattern, game data and user
    // directory merged (the user directory's copy wins), sorted by lower-case name.
    std::vector<std::string> ListFiles(const std::string &dataDir, const std::string &userDir,
                                       const char *directory, const char *pattern);
    // The edition to use: configured, unless dataDir has no CountrySpecific
    // folder for it and has one for a single other edition.
    // Returns configured or a constant.
    const char *DataSku(const std::string &dataDir, const char *configured);

private:
    Host &_host;
    std::map<std::string, Entries> _dirs;
};

} // namespace SysPaths

#endif
