#include "platform/text_entry.h"

namespace TextEntry {

namespace {

// The characters of Windows-1252's 0x80 to 0x9f (0: none). The rest are Latin-1's.
const uint16_t kWindows1252High[32] = { 0x20ac, 0, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021,
                                        0x02c6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017d, 0,
                                        0, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014,
                                        0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0, 0x017e, 0x0178 };

} // namespace

int ToWindows1252(uint32_t codePoint)
{
    if (codePoint < 0x80 || (codePoint >= 0xa0 && codePoint <= 0xff))
        return (int)codePoint;
    for (int i = 0; i < 32; i++)
        if (kWindows1252High[i] == codePoint)
            return 0x80 + i;
    return 0;
}

uint32_t FromWindows1252(unsigned char c)
{
    return c >= 0x80 && c < 0xa0 ? kWindows1252High[c - 0x80] : c;
}

std::u16string Utf16FromWindows1252(const std::string &text)
{
    std::u16string out;
    for (unsigned char c : text) {
        const uint32_t codePoint = FromWindows1252(c);
        if (codePoint != 0)
            out += (char16_t)codePoint;     // all below 0x10000
    }
    return out;
}

std::vector<uint32_t> CodePointsFromUtf16(const char16_t *text)
{
    std::vector<uint32_t> out;
    for (; *text != 0; text++) {
        const uint32_t unit = *text;
        if (unit >= 0xd800 && unit < 0xdc00 && text[1] >= 0xdc00 && text[1] < 0xe000) {
            out.push_back(0x10000 + ((unit - 0xd800) << 10) + (text[1] - 0xdc00));
            text++;
        } else {
            out.push_back(unit);
        }
    }
    return out;
}

std::vector<int> ReplaceKeys(size_t oldLength, const std::vector<uint32_t> &answer)
{
    std::vector<int> keys(oldLength, kBackspace);
    for (uint32_t codePoint : answer)
        if (const int c = ToWindows1252(codePoint))
            keys.push_back(c);
    keys.push_back(kEnter);
    return keys;
}

} // namespace TextEntry
