#pragma once
// The core string type and frontend-boundary conversions (macOS impl guide,
// Phase 0.5 "core string type").
//
// The core stores text as 16-bit UTF-16 everywhere:
//   - Windows: std::wstring (wchar_t is already 16-bit UTF-16), so the Win32/GDI
//     frontend keeps using it directly with zero conversions.
//   - macOS/Linux: std::u16string (char16_t), which maps directly to CoreText
//     `unichar` and avoids the 32-bit-wchar_t / UTF-32 bloat std::wstring would
//     otherwise incur off Windows.
//
// Core code writes `Str`, `Char`, and `U16("literal")` instead of `std::wstring`,
// `wchar_t`, and `L"literal"`; on Windows those expand back to the exact wchar_t
// forms, so the Win32 build is unchanged.
#include <string>
#include <string_view>

#if defined(_WIN32)
using Str = std::wstring;
using StrView = std::wstring_view;
#define U16(x) L##x
#else
using Str = std::u16string;
using StrView = std::u16string_view;
#define U16(x) u##x
#endif

using Char = Str::value_type;  // wchar_t on Windows, char16_t elsewhere (both 16-bit)

// Integer -> Str (ASCII digits). Replaces std::to_wstring in core code.
Str toStr(long n);

// UTF-8 <-> core string. Used by the CLI and the future macOS frontend; the
// Win32 frontend uses the Win32 helpers below (or Str directly, since Str is
// std::wstring there).
Str FromUtf8(std::string_view utf8);
std::string ToUtf8(StrView s);

#if defined(_WIN32)
// On Windows Str already IS std::wstring, so these are effectively identity —
// provided so frontend code can be explicit about crossing the boundary.
inline Str FromWin32String(std::wstring_view s) { return Str(s); }
inline std::wstring ToWin32String(StrView s) { return std::wstring(s); }
#endif
