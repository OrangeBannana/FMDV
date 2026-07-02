#pragma once
#include <windows.h>

struct Theme {
    COLORREF bg;        // page background
    COLORREF bg2;       // code/quote/table-stripe background
    COLORREF bg3;       // editor textarea background
    COLORREF text;      // body text
    COLORREF text2;     // muted (blockquote, table sep)
    COLORREF border;    // rules, table borders, h1/h2 underline
    COLORREF link;      // link text
    COLORREF codeText;  // monospace text
    COLORREF sel;       // text-selection highlight
};

inline Theme LightTheme() {
    return Theme{
        RGB(0xff, 0xff, 0xff), // bg
        RGB(0xf6, 0xf8, 0xfa), // bg2
        RGB(0xfa, 0xfb, 0xfc), // bg3
        RGB(0x24, 0x29, 0x2f), // text
        RGB(0x57, 0x60, 0x6a), // text2
        RGB(0xd0, 0xd7, 0xde), // border
        RGB(0x09, 0x69, 0xda), // link
        RGB(0x24, 0x29, 0x2f), // codeText
        RGB(0xae, 0xd4, 0xfb), // sel
    };
}

inline Theme DarkTheme() {
    return Theme{
        RGB(0x0d, 0x11, 0x17), // bg
        RGB(0x16, 0x1b, 0x22), // bg2
        RGB(0x16, 0x1b, 0x22), // bg3
        RGB(0xe6, 0xed, 0xf3), // text
        RGB(0x8b, 0x94, 0x9e), // text2
        RGB(0x30, 0x36, 0x3d), // border
        RGB(0x58, 0xa6, 0xff), // link
        RGB(0xe6, 0xed, 0xf3), // codeText
        RGB(0x26, 0x4f, 0x78), // sel
    };
}
