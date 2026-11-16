// The default settings of the PS Vita, and the first opencmr2.ini it writes.
#ifndef OPENCMR2_VITA_SETTINGS_H
#define OPENCMR2_VITA_SETTINGS_H

#include <string>

namespace VitaSettings {

// An ini text: the comments and the Vita's values. Parsed, it gives the
// defaults. CommentedOut of it is the file a first run writes.
extern const char kDefaults[];

// The text with each "key = value" line as "; key = value": the file shows
// the defaults, and a later default still applies to the keys it does not set.
std::string CommentedOut(const std::string &text);

// The game's language for a system language (SCE_SYSTEM_PARAM_LANG_*).
// English for a language the game's European data does not have.
const char *Language(int systemLanguage);

} // namespace VitaSettings

#endif
