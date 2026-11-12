// Game paths on the host file system: see sys_paths.h.

#include "platform/sys_paths.h"

namespace SysPaths {

std::string Lower(const std::string &s)
{
    std::string out = s;
    for (char &c : out) {
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
    }
    return out;
}

std::vector<std::string> SplitGamePath(const char *path)
{
    std::vector<std::string> parts;
    const char *p = path;

    if (p[0] && p[1] == ':')
        p += 2;
    std::string part;
    for (;; p++) {
        if (*p == '\\' || *p == '/' || *p == 0) {
            if (part == "..") {
                if (!parts.empty())
                    parts.pop_back();
            } else if (!part.empty() && part != ".") {
                parts.push_back(part);
            }
            part.clear();
            if (*p == 0)
                break;
        } else {
            part += *p;
        }
    }
    return parts;
}

bool MatchPattern(const std::string &name, const std::string &pattern)
{
    std::string n = Lower(name), p = Lower(pattern);
    size_t star = p.find('*');
    if (star == std::string::npos)
        return n == p;
    std::string head = p.substr(0, star), tail = p.substr(star + 1);
    return n.size() >= head.size() + tail.size() && n.compare(0, head.size(), head) == 0 &&
           n.compare(n.size() - tail.size(), tail.size(), tail) == 0;
}

std::string WithSlash(std::string dir)
{
    if (!dir.empty() && dir.back() != '/')
        dir += '/';
    return dir;
}

const Entries &Cache::List(const std::string &dir)
{
    auto it = _dirs.find(dir);
    if (it != _dirs.end())
        return it->second;
    Entries &entries = _dirs[dir];
    _host.List(dir, entries);
    return entries;
}

std::string Cache::FindUnder(const std::string &root, const std::vector<std::string> &parts)
{
    std::string path = root;
    for (size_t i = 0; i < parts.size(); i++) {
        const Entries &entries = List(path);
        auto it = entries.find(Lower(parts[i]));
        if (it == entries.end())
            return std::string();
        path += it->second;
        if (i + 1 < parts.size())
            path += '/';
    }
    return path;
}

std::string Cache::PathForWriting(const std::string &userDir, const std::vector<std::string> &parts)
{
    std::string path = userDir;
    for (size_t i = 0; i < parts.size(); i++) {
        const Entries &entries = List(path);
        auto it = entries.find(Lower(parts[i]));
        std::string name = it != entries.end() ? it->second : parts[i];
        bool isDir = i + 1 < parts.size();
        if (it == entries.end()) {
            if (isDir)
                _host.MakeDirectory(path + name);
            _dirs[path].emplace(Lower(name), name);
        }
        path += name;
        if (isDir)
            path += '/';
    }
    return path;
}

std::string Cache::ResolveForReading(const std::string &userDir, const std::string &dataDir, const char *gamePath)
{
    std::vector<std::string> parts = SplitGamePath(gamePath);
    if (parts.empty())
        return std::string();
    std::string found = FindUnder(userDir, parts);
    if (found.empty())
        found = FindUnder(dataDir, parts);
    return found;
}

std::string Cache::MissingGameData(const std::string &dir)
{
    std::string missing;
    // These archives are common to the PC editions; folder names alone also
    // match empty directories and unextracted installer media.
    for (const char *file : {"Game/Bigfiles/common.bfl", "FrontEnd/common.bfl", "Sounds/surface.bfl"}) {
        std::string path = FindUnder(dir, SplitGamePath(file));
        uint64_t size = 0;
        if (path.empty() || _host.GetKind(path, &size) != Kind::File || size == 0)
            missing += std::string("\n- ") + file;
    }
    // The text, speech and loading screens are in these folders, which some
    // copies of an installation leave out; the game cannot start without them.
    for (const char *folder : {"CountrySpecific", "SetupRep"}) {
        std::string path = FindUnder(dir, {folder});
        if (path.empty() || _host.GetKind(path, NULL) != Kind::Directory || List(path + "/").empty())
            missing += std::string("\n- ") + folder + "/";
    }
    return missing;
}

std::vector<std::string> Cache::ListFiles(const std::string &dataDir, const std::string &userDir,
                                          const char *directory, const char *pattern)
{
    std::vector<std::string> parts = SplitGamePath(directory);
    Entries names;
    for (const std::string *root : {&dataDir, &userDir}) {
        std::string dir = parts.empty() ? *root : FindUnder(*root, parts);
        if (dir.empty())
            continue;
        dir = WithSlash(dir);
        for (const auto &entry : List(dir)) {
            if (MatchPattern(entry.second, pattern) && _host.GetKind(dir + entry.second, NULL) == Kind::File)
                names[entry.first] = entry.second;
        }
    }
    std::vector<std::string> out;
    for (const auto &name : names)
        out.push_back(name.second);
    return out;
}

const char *Cache::DataSku(const std::string &dataDir, const char *configured)
{
    static const char *const skus[][2] = {
        {"EUROPE", "Europe"}, {"AMERICA", "Usa"}, {"JAPAN", "Japan"}, {"POLAND", "Poland"}};
    if (dataDir.empty())
        return configured;
    auto present = [&](const char *folder) {
        std::string path = FindUnder(dataDir, {"CountrySpecific", folder});
        return !path.empty() && _host.GetKind(path, NULL) == Kind::Directory;
    };
    const char *found = NULL;
    for (const auto &sku : skus) {
        if (Lower(sku[0]) == Lower(configured) && present(sku[1]))
            return configured;
        if (present(sku[1]))
            found = found == NULL ? sku[0] : "";
    }
    if (found == NULL || *found == 0)
        return configured;
    return found;
}

} // namespace SysPaths
