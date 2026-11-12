// Settings text and the step clock, shared by the platform backends.

#include "platform/sys_common.h"
#include "platform/sys_paths.h"
#include "port/sys.h"

#include <sstream>
#include <vector>

namespace SysSettings {

void Parse(const std::string &text, Values &values)
{
    std::istringstream input(text);
    std::string line;
    std::string section;
    while (std::getline(input, line)) {
        std::string s = line;
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
            s.pop_back();
        size_t start = s.find_first_not_of(" \t");
        if (start == std::string::npos || s[start] == ';' || s[start] == '#')
            continue;
        s = s.substr(start);
        if (s[0] == '[') {
            section = SysPaths::Lower(s.substr(1, s.find(']') - 1));
            continue;
        }
        size_t eq = s.find('=');
        if (eq == std::string::npos)
            continue;
        std::string key = s.substr(0, eq), value = s.substr(eq + 1);
        while (!key.empty() && key.back() == ' ')
            key.pop_back();
        value.erase(0, value.find_first_not_of(' '));
        values[section + "." + SysPaths::Lower(key)] = value;
    }
}

std::string Edit(const std::string &text, const std::string &section, const std::string &key,
                 const std::string &value)
{
    std::istringstream input(text);
    const std::string setting = key + " = " + value;
    std::vector<std::string> lines;
    std::string line, current;
    size_t headerEnd = std::string::npos;
    bool written = false;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        size_t start = line.find_first_not_of(" \t");
        std::string trimmed = start == std::string::npos ? "" : line.substr(start);
        if (!trimmed.empty() && trimmed[0] == '[') {
            current = SysPaths::Lower(trimmed.substr(1, trimmed.find(']') - 1));
            lines.push_back(line);
            if (current == section && headerEnd == std::string::npos)
                headerEnd = lines.size();
            continue;
        }
        size_t eq = trimmed.find('=');
        if (current == section && eq != std::string::npos) {
            std::string name = trimmed.substr(0, eq);
            size_t end = name.find_last_not_of(" \t");
            if (end != std::string::npos && SysPaths::Lower(name.substr(0, end + 1)) == key) {
                if (!written)
                    lines.push_back(setting);
                written = true;
                continue;
            }
        }
        lines.push_back(line);
    }
    if (!written && headerEnd != std::string::npos)
        lines.insert(lines.begin() + headerEnd, setting);
    else if (!written)
        lines.insert(lines.end(), {"", "[" + section + "]", setting});
    std::string output;
    for (const std::string &l : lines)
        output += l + '\n';
    return output;
}

} // namespace SysSettings

extern "C" int Sys_TakeSteps(SysStepClock *clock, int rate, int max)
{
    const DWORD now = Sys_GetTicks();
    const DWORD elapsed = now - clock->last;
    clock->last = now;
    if (!clock->started || elapsed > 250) {
        clock->started = 1;
        clock->remainder = 0;
        return 1;
    }
    const DWORD units = clock->remainder + elapsed * (DWORD)rate;
    clock->remainder = units % 1000;
    const int steps = (int)(units / 1000);
    return steps < max ? steps : max;
}
