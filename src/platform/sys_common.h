// OpenCMR2 settings text (opencmr2.ini), shared by sys.cpp (SDL) and
// sys_vita.cpp. Each backend reads and writes the file itself.
#ifndef OPENCMR2_SYS_COMMON_H
#define OPENCMR2_SYS_COMMON_H

#include <map>
#include <string>

namespace SysSettings {

// "section.key" (lower case) -> value.
typedef std::map<std::string, std::string> Values;

// Adds the settings of an ini text to values.
void Parse(const std::string &text, Values &values);
// The text with "key = value" in [section]: the first such line is replaced
// in place and repeats (in any [section] block) are dropped. Unrelated
// settings and comments are kept. section and key are lower case.
std::string Edit(const std::string &text, const std::string &section, const std::string &key,
                 const std::string &value);

} // namespace SysSettings

#endif
