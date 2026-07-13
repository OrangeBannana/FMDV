#pragma once
#include <vector>
#include "str.h"

enum class BlockType {
    Heading,     // level 1-6, runs
    Paragraph,   // runs
    CodeBlock,   // lang, codeText
    BlockQuote,  // runs (joined)
    ListItem,    // ordered, level(depth), taskState, runs
    Table,       // headers, rows, aligns
    HRule
};

enum Align { AlignLeft = 0, AlignCenter = 1, AlignRight = 2 };

struct InlineRun {
    Str text;
    bool bold = false;
    bool italic = false;
    bool code = false;
    bool strike = false;
    Str href; // non-empty => link
};

struct TableCell { std::vector<InlineRun> runs; };
struct TableRow  { std::vector<TableCell> cells; };

struct Block {
    BlockType type = BlockType::Paragraph;
    int level = 0;           // heading level (1-6) or list depth (0-based)
    bool ordered = false;    // ordered list item
    int taskState = -1;      // -1 none, 0 unchecked, 1 checked
    Str lang;       // code block language
    Str codeText;   // raw code block contents
    std::vector<InlineRun> runs;          // inline content
    std::vector<TableCell> headers;       // table header cells
    std::vector<TableRow> rows;           // table body rows
    std::vector<int> aligns;              // table column alignments
    // 0-based source line range (inclusive), -1 = unset. Set for Table blocks
    // (whole pipe grid) and for ListItem blocks (the single item line, in
    // srcStartLine). Lets editor/viewer features rewrite a block's markdown in
    // place: the table-resize picker (fmdv.cpp) and the clickable task-checkbox
    // toggle both use it.
    int srcStartLine = -1;
    int srcEndLine = -1;
};

struct Document {
    std::vector<Block> blocks;
};

// Parse normalized UTF-16 markdown (LF line endings) into a Document.
Document ParseMarkdown(const Str& text);

// Parse a single line of inline markdown into styled runs.
std::vector<InlineRun> ParseInline(const Str& text);

// Split a raw table row/header line on unescaped '|' into trimmed cell text
// (no inline parsing — markdown syntax in a cell is left intact). Exposed so
// editor features can rewrite a table's source without losing formatting
// that ParseInline would have already converted into InlineRun style flags.
std::vector<Str> SplitTableCells(const Str& line);
