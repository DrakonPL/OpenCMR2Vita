// The game's text entries edited in a system dialog: the conversions between
// the game's Windows-1252 and the dialog's Unicode, and the keys that type the
// dialog's answer into the entry. No platform here.
#ifndef OPENCMR2_TEXT_ENTRY_H
#define OPENCMR2_TEXT_ENTRY_H

#include <stdint.h>

#include <string>
#include <vector>

namespace TextEntry {

constexpr int kBackspace = 8;
constexpr int kEnter = 13;

// 0 for a character that Windows-1252 does not have.
int ToWindows1252(uint32_t codePoint);
uint32_t FromWindows1252(unsigned char c);

std::u16string Utf16FromWindows1252(const std::string &text);
// The characters of a zero-ended UTF-16 text, its surrogate pairs joined.
std::vector<uint32_t> CodePointsFromUtf16(const char16_t *text);

// The characters that replace an entry's text with the answer, as typed: a
// Backspace for each old character, the new ones that Windows-1252 has, then
// Enter. A chat line sends on the Enter. A name entry ignores it.
std::vector<int> ReplaceKeys(size_t oldLength, const std::vector<uint32_t> &answer);

} // namespace TextEntry

#endif
