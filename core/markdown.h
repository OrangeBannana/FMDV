#pragma once
#include <string>
#include <vector>

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
    std::wstring text;
    bool bold = false;
    bool italic = false;
    bool code = false;
    bool strike = false;
    std::wstring href; // non-empty => link
};

struct TableCell { std::vector<InlineRun> runs; };
struct TableRow  { std::vector<TableCell> cells; };

struct Block {
    BlockType type = BlockType::Paragraph;
    int level = 0;           // heading level (1-6) or list depth (0-based)
    bool ordered = false;    // ordered list item
    int taskState = -1;      // -1 none, 0 unchecked, 1 checked
    std::wstring lang;       // code block language
    std::wstring codeText;   // raw code block contents
    std::vector<InlineRun> runs;          // inline content
    std::vector<TableCell> headers;       // table header cells
    std::vector<TableRow> rows;           // table body rows
    std::vector<int> aligns;              // table column alignments
    // 0-based source line range (inclusive), Table blocks only (-1 = unset).
    // Lets editor features rewrite a table's markdown in place without
    // re-parsing pipe syntax ad hoc (see fmdv.cpp's table-resize picker).
    int srcStartLine = -1;
    int srcEndLine = -1;
};

struct Document {
    std::vector<Block> blocks;
};

// Parse normalized UTF-16 markdown (LF line endings) into a Document.
Document ParseMarkdown(const std::wstring& text);

// Parse a single line of inline markdown into styled runs.
std::vector<InlineRun> ParseInline(const std::wstring& text);

// Split a raw table row/header line on unescaped '|' into trimmed cell text
// (no inline parsing — markdown syntax in a cell is left intact). Exposed so
// editor features can rewrite a table's source without losing formatting
// that ParseInline would have already converted into InlineRun style flags.
std::vector<std::wstring> SplitTableCells(const std::wstring& line);
