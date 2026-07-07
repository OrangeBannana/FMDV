#include "str.h"

Str toStr(long n) {
    std::string s = std::to_string(n);   // ASCII digits (and '-')
    Str o;
    o.reserve(s.size());
    for (char c : s) o.push_back((Char)(unsigned char)c);
    return o;
}

static void appendUnit(Str& w, char32_t cp) {
    if (sizeof(Char) >= 4) { w.push_back((Char)cp); return; }
    if (cp <= 0xFFFF) {
        w.push_back((Char)cp);
    } else {                              // encode astral as a UTF-16 surrogate pair
        cp -= 0x10000;
        w.push_back((Char)(0xD800 + (cp >> 10)));
        w.push_back((Char)(0xDC00 + (cp & 0x3FF)));
    }
}

Str FromUtf8(std::string_view s) {
    Str w;
    w.reserve(s.size());
    size_t i = 0, n = s.size();
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        char32_t cp;
        int len;
        if (c < 0x80)             { cp = c;        len = 1; }
        else if ((c >> 5) == 0x6) { cp = c & 0x1F; len = 2; }
        else if ((c >> 4) == 0xE) { cp = c & 0x0F; len = 3; }
        else if ((c >> 3) == 0x1E){ cp = c & 0x07; len = 4; }
        else                      { cp = 0xFFFD;   len = 1; }
        if (i + (size_t)len > n)  { cp = 0xFFFD;   len = 1; }
        for (int k = 1; k < len; k++) {
            unsigned char cc = (unsigned char)s[i + k];
            if ((cc & 0xC0) != 0x80) { cp = 0xFFFD; len = 1; break; }
            cp = (cp << 6) | (cc & 0x3F);
        }
        appendUnit(w, cp);
        i += (size_t)len;
    }
    return w;
}

static void appendUtf8(std::string& o, char32_t cp) {
    if (cp < 0x80) {
        o += (char)cp;
    } else if (cp < 0x800) {
        o += (char)(0xC0 | (cp >> 6));
        o += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        o += (char)(0xE0 | (cp >> 12));
        o += (char)(0x80 | ((cp >> 6) & 0x3F));
        o += (char)(0x80 | (cp & 0x3F));
    } else {
        o += (char)(0xF0 | (cp >> 18));
        o += (char)(0x80 | ((cp >> 12) & 0x3F));
        o += (char)(0x80 | ((cp >> 6) & 0x3F));
        o += (char)(0x80 | (cp & 0x3F));
    }
}

std::string ToUtf8(StrView s) {
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        char32_t cp = (char32_t)(unsigned long)s[i];
        if (sizeof(Char) < 4 && cp >= 0xD800 && cp <= 0xDBFF && i + 1 < s.size()) {
            char32_t lo = (char32_t)(unsigned long)s[i + 1];
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                i++;
            }
        }
        appendUtf8(o, cp);
    }
    return o;
}
