#pragma once
// Platform-neutral editor decision helpers (macOS impl guide, "core/edit-helpers").
//
// These are the pure parts of the source editor: given the current line (or a
// size), decide what to do. They perform no I/O and touch no UI — the frontend
// reads the text, calls these, and applies the result to its own edit control
// (Win32 EDIT, NSTextView, ...). Shared by the Win32 app, the CLI, and the
// future macOS frontend so all three behave identically.
//
// Text is the core Str type (16-bit UTF-16). The logic is code-unit based and
// only inspects ASCII markdown syntax, so it behaves identically everywhere.
#include "str.h"

namespace fmdv {

// ---- markdown autocomplete (ghost text) ----
// Given the current line up to the caret, suggest a closing/companion string.
// `text` is the overlay to show (and to insert on Tab); it may contain '\n',
// which the frontend expands to its own line ending. `caret` is where the caret
// should land within the inserted text after commit. Empty text => no suggestion.
struct Suggestion {
    Str text;
    int caret = 0;
};
Suggestion SuggestClose(const Str& line);

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
    Str continuation; // indent + marker, no line ending
};
ListEnter DecideListEnter(const Str& line);

// ---- table insertion ----
// Markdown for a cols x rows table with placeholder headers ("Column N") and
// empty body cells. LF line endings, one trailing newline, no leading newline;
// the frontend converts line endings and handles caret positioning.
Str MakeTableMarkdown(int cols, int rows);

} // namespace fmdv
