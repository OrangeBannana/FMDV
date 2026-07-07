#pragma once
// Platform-neutral editor decision helpers (macOS impl guide, "core/edit-helpers").
//
// These are the pure parts of the source editor: given the current line (or a
// size), decide what to do. They perform no I/O and touch no UI — the frontend
// reads the text, calls these, and applies the result to its own edit control
// (Win32 EDIT, NSTextView, ...). Shared by the Win32 app, the CLI, and the
// future macOS frontend so all three behave identically.
//
// Strings are std::wstring for now, matching the parser; they migrate to the
// core Str type together in the string-type milestone. The logic is code-point
// based and behaves the same whether wchar_t is 16- or 32-bit.
#include <string>

namespace fmdv {

// ---- markdown autocomplete (ghost text) ----
// Given the current line up to the caret, suggest a closing/companion string.
// `text` is the overlay to show (and to insert on Tab); it may contain '\n',
// which the frontend expands to its own line ending. `caret` is where the caret
// should land within the inserted text after commit. Empty text => no suggestion.
struct Suggestion {
    std::wstring text;
    int caret = 0;
};
Suggestion SuggestClose(const std::wstring& line);

// ---- list continuation on Enter ----
// Decide what pressing Enter inside `line` (the current line up to the caret)
// should do for bullet / ordered / task-list items:
//   handled == false  -> not a list item; let the default newline happen.
//   endList == true    -> the item is empty; the frontend clears the marker
//                         (the line's text from its start to the caret).
//   otherwise          -> continue the list: the frontend inserts a newline
//                         followed by `continuation` (indent + next marker).
struct ListEnter {
    bool handled = false;
    bool endList = false;
    std::wstring continuation; // indent + marker, no line ending
};
ListEnter DecideListEnter(const std::wstring& line);

// ---- table insertion ----
// Markdown for a cols x rows table with placeholder headers ("Column N") and
// empty body cells. LF line endings, one trailing newline, no leading newline;
// the frontend converts line endings and handles caret positioning.
std::wstring MakeTableMarkdown(int cols, int rows);

} // namespace fmdv
