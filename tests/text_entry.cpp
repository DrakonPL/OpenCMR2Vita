// The answer of a text dialog typed into a game entry
// (src/platform/text_entry.cpp): the keys, and the conversions between the
// game's Windows-1252 and the dialog's UTF-16.

#include "platform/text_entry.h"

#include <cstdio>
#include <string>
#include <vector>

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "failed line %d: %s\n", __LINE__, #x); return 1; } } while (0)

namespace {

int TestReplaceKeys()
{
    using TextEntry::kBackspace;
    using TextEntry::kEnter;
    // The old text "abc", the answer "de".
    const std::vector<int> keys = TextEntry::ReplaceKeys(3, { 'd', 'e' });
    REQUIRE((keys == std::vector<int>{ kBackspace, kBackspace, kBackspace, 'd', 'e', kEnter }));
    // A character that Windows-1252 does not have is left out. The euro sign is 0x80.
    REQUIRE((TextEntry::ReplaceKeys(0, { 0x20ac, 0x4e2d, 'x' }) == std::vector<int>{ 0x80, 'x', kEnter }));
    // An empty answer clears the entry.
    REQUIRE((TextEntry::ReplaceKeys(2, {}) == std::vector<int>{ kBackspace, kBackspace, kEnter }));
    return 0;
}

int TestConversions()
{
    // "é" is 0xe9 in both. "Š" is 0x8a in Windows-1252, U+0160 in Unicode.
    REQUIRE(TextEntry::Utf16FromWindows1252("V\xe9\x8a") == u"VéŠ");
    REQUIRE(TextEntry::ToWindows1252(0x0160) == 0x8a);
    REQUIRE(TextEntry::ToWindows1252(0x81) == 0);
    REQUIRE(TextEntry::FromWindows1252(0x8a) == 0x0160);
    // A surrogate pair is one character.
    const std::vector<uint32_t> points = TextEntry::CodePointsFromUtf16(u"A\U0001F600B");
    REQUIRE((points == std::vector<uint32_t>{ 'A', 0x1f600, 'B' }));
    REQUIRE(TextEntry::CodePointsFromUtf16(u"").empty());
    return 0;
}

} // namespace

int main()
{
    if (TestReplaceKeys() || TestConversions())
        return 1;
    printf("text_entry: ok\n");
    return 0;
}
