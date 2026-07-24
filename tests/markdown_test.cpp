// Unit tests for the shared markdown parser (core/markdown): block structure,
// inline styling, and table splitting. This is the behavior both frontends
// render from, so it gets the deepest coverage of the core suites.
#include "markdown.h"
#include "test_check.h"
#include <string>

static std::string u8(const Str& s) { return ToUtf8(s); }
static Document parse(const char* utf8) { return ParseMarkdown(FromUtf8(utf8)); }

// Concatenate a run list's text (ignoring styling).
static std::string runText(const std::vector<InlineRun>& runs) {
    std::string o;
    for (const auto& r : runs) o += u8(r.text);
    return o;
}

int main() {
    // ---- headings ----
    {
        Document d = parse("# One\n###### Six");
        check(d.blocks.size() == 2, "heading: two blocks");
        check(d.blocks[0].type == BlockType::Heading && d.blocks[0].level == 1,
              "heading: level 1");
        check(runText(d.blocks[0].runs) == "One", "heading: text");
        check(d.blocks[1].type == BlockType::Heading && d.blocks[1].level == 6,
              "heading: level 6");
    }
    {
        Document d = parse("####### seven");
        check(d.blocks.size() == 1 && d.blocks[0].type == BlockType::Paragraph,
              "heading: 7 hashes is a paragraph");
        Document e = parse("#nospace");
        check(e.blocks.size() == 1 && e.blocks[0].type == BlockType::Paragraph,
              "heading: no space after # is a paragraph");
    }

    // ---- paragraphs ----
    {
        Document d = parse("line one\nline two\n\nnext para");
        check(d.blocks.size() == 2, "para: blank line separates paragraphs");
        check(runText(d.blocks[0].runs) == "line one line two",
              "para: adjacent lines join with a space");
        check(runText(d.blocks[1].runs) == "next para", "para: second paragraph text");
    }
    {
        Document d = parse("   \n\n");
        check(d.blocks.empty(), "para: whitespace-only input yields no blocks");
    }

    // ---- inline styling ----
    {
        auto runs = ParseInline(FromUtf8("before **b** after"));
        check(runs.size() == 3, "inline: bold splits into three runs");
        check(runs.size() == 3 && !runs[0].bold && runs[1].bold && !runs[2].bold,
              "inline: only middle run is bold");
        check(runs.size() == 3 && u8(runs[0].text) == "before " && u8(runs[1].text) == "b"
                  && u8(runs[2].text) == " after",
              "inline: bold run boundaries keep spacing");
    }
    {
        auto runs = ParseInline(FromUtf8("*i* __B__ ~~s~~ `c`"));
        check(runs.size() >= 4, "inline: mixed styles parse");
        bool i = false, b = false, s = false, c = false;
        for (const auto& r : runs) {
            if (u8(r.text) == "i") i = r.italic && !r.bold;
            if (u8(r.text) == "B") b = r.bold && !r.italic;
            if (u8(r.text) == "s") s = r.strike;
            if (u8(r.text) == "c") c = r.code;
        }
        check(i, "inline: * italic");
        check(b, "inline: __ bold");
        check(s, "inline: ~~ strike");
        check(c, "inline: ` code");
    }
    {
        auto runs = ParseInline(FromUtf8("***both***"));
        check(runs.size() == 1 && runs[0].bold && runs[0].italic && u8(runs[0].text) == "both",
              "inline: *** is bold+italic");
    }
    {
        auto runs = ParseInline(FromUtf8("_a **b** c_"));
        check(runs.size() == 3, "inline: nesting splits runs");
        check(runs.size() == 3 && runs[0].italic && !runs[0].bold
                  && runs[1].italic && runs[1].bold && runs[2].italic && !runs[2].bold,
              "inline: bold nests inside italic");
    }
    {
        auto runs = ParseInline(FromUtf8("`**not bold**`"));
        check(runs.size() == 1 && runs[0].code && !runs[0].bold
                  && u8(runs[0].text) == "**not bold**",
              "inline: no styling inside inline code");
    }
    {
        auto runs = ParseInline(FromUtf8("**unclosed"));
        check(runs.size() == 1 && !runs[0].bold && u8(runs[0].text) == "**unclosed",
              "inline: unclosed bold stays literal");
        auto r2 = ParseInline(FromUtf8("a * b"));
        check(runText(r2) == "a * b", "inline: stray asterisk stays literal");
    }
    {
        auto runs = ParseInline(FromUtf8("go [here](https://e.com) now"));
        check(runs.size() == 3, "inline: link splits runs");
        check(runs.size() == 3 && u8(runs[1].text) == "here"
                  && u8(runs[1].href) == "https://e.com",
              "inline: link text and href");
        check(runs.size() == 3 && runs[0].href.empty() && runs[2].href.empty(),
              "inline: neighbors have no href");
        auto s2 = ParseInline(FromUtf8("[**bold link**](u)"));
        check(s2.size() == 1 && s2[0].bold && u8(s2[0].href) == "u",
              "inline: styled link text keeps href");
    }
    {
        auto runs = ParseInline(FromUtf8("![alt text](pic.png)"));
        check(runs.size() == 1 && u8(runs[0].text) == "alt text" && runs[0].href.empty(),
              "inline: image renders alt text");
        auto r2 = ParseInline(FromUtf8("![](pic.png)"));
        check(r2.size() == 1 && u8(r2[0].text) == "[image]",
              "inline: empty alt renders [image]");
    }
    {
        // <br> is GFM's escape hatch for a hard break inside a table cell
        // (pipe-table syntax can't hold a literal newline); folded to '\n' here,
        // which layout.cpp turns into an actual forced line break.
        auto runs = ParseInline(FromUtf8("one<br>two"));
        check(runs.size() == 1 && u8(runs[0].text) == "one\ntwo", "br: folds to a newline");
        check(u8(ParseInline(FromUtf8("a<br/>b"))[0].text) == "a\nb", "br: self-closing <br/>");
        check(u8(ParseInline(FromUtf8("a<br />b"))[0].text) == "a\nb", "br: spaced self-close <br />");
        check(u8(ParseInline(FromUtf8("a<BR>b"))[0].text) == "a\nb", "br: case-insensitive");
        auto two = ParseInline(FromUtf8("a<br><br>b"));
        check(runText(two) == "a\n\nb", "br: back-to-back <br><br> is a blank line");
        auto bold = ParseInline(FromUtf8("**a<br>b**"));
        check(bold.size() == 1 && bold[0].bold && u8(bold[0].text) == "a\nb",
              "br: works inside styled text");
    }
    {
        auto runs = ParseInline(FromUtf8("[not a link] plain"));
        check(runs.size() == 1 && u8(runs[0].text) == "[not a link] plain",
              "inline: bracket without (url) stays literal");
    }

    // ---- fenced code blocks ----
    {
        Document d = parse("```python\ndef f():\n    return 1\n```\nafter");
        check(d.blocks.size() == 2, "code: fence closes");
        check(d.blocks[0].type == BlockType::CodeBlock, "code: block type");
        check(u8(d.blocks[0].lang) == "python", "code: language");
        check(u8(d.blocks[0].codeText) == "def f():\n    return 1",
              "code: raw text with indentation");
        check(d.blocks[1].type == BlockType::Paragraph, "code: parsing resumes after fence");
    }
    {
        Document d = parse("~~~\nx\n~~~");
        check(d.blocks.size() == 1 && d.blocks[0].type == BlockType::CodeBlock
                  && u8(d.blocks[0].codeText) == "x",
              "code: ~~~ fence works");
    }
    {
        Document d = parse("```\n**not bold**\n- not a list\n```");
        check(d.blocks.size() == 1 && u8(d.blocks[0].codeText) == "**not bold**\n- not a list",
              "code: contents are not parsed as markdown");
    }
    {
        Document d = parse("```\nunterminated\nstill code");
        check(d.blocks.size() == 1 && d.blocks[0].type == BlockType::CodeBlock
                  && u8(d.blocks[0].codeText) == "unterminated\nstill code",
              "code: unterminated fence runs to EOF");
    }

    // ---- horizontal rules ----
    {
        Document d = parse("---\n***\n___\n- - -");
        check(d.blocks.size() == 4, "hrule: four rules");
        bool all = true;
        for (const auto& b : d.blocks) all = all && b.type == BlockType::HRule;
        check(all, "hrule: ---, ***, ___, and spaced dashes");
    }
    {
        Document d = parse("--");
        check(d.blocks.size() == 1 && d.blocks[0].type == BlockType::Paragraph,
              "hrule: two dashes is not a rule");
        Document e = parse("***x***");
        check(e.blocks.size() == 1 && e.blocks[0].type == BlockType::Paragraph,
              "hrule: ***text*** is a paragraph, not a rule");
    }

    // ---- blockquotes ----
    {
        Document d = parse("> first\n> second\nplain");
        check(d.blocks.size() == 2, "quote: quote then paragraph");
        check(d.blocks[0].type == BlockType::BlockQuote, "quote: block type");
        check(runText(d.blocks[0].runs) == "first second",
              "quote: consecutive lines join with a space");
        check(d.blocks[1].type == BlockType::Paragraph, "quote: ends at non-quote line");
    }

    // ---- lists ----
    {
        Document d = parse("- a\n* b\n+ c");
        check(d.blocks.size() == 3, "list: three items");
        bool all = true;
        for (const auto& b : d.blocks)
            all = all && b.type == BlockType::ListItem && !b.ordered && b.taskState == -1;
        check(all, "list: -, *, + all bullet");
    }
    {
        Document d = parse("1. one\n10. ten");
        check(d.blocks.size() == 2 && d.blocks[0].ordered && d.blocks[1].ordered,
              "list: ordered items");
        check(runText(d.blocks[1].runs) == "ten", "list: multi-digit marker stripped");
    }
    {
        Document d = parse("- top\n  - nested\n    - deeper");
        check(d.blocks.size() == 3, "list: nesting parses");
        check(d.blocks[0].level == 0 && d.blocks[1].level == 1 && d.blocks[2].level == 2,
              "list: two spaces per depth level");
    }
    {
        Document d = parse("- [ ] todo\n- [x] done\n- [X] also done");
        check(d.blocks.size() == 3, "task: three items");
        check(d.blocks[0].taskState == 0, "task: [ ] unchecked");
        check(d.blocks[1].taskState == 1 && d.blocks[2].taskState == 1,
              "task: [x] and [X] checked");
        check(runText(d.blocks[1].runs) == "done", "task: marker stripped from text");
    }
    {
        Document d = parse("-notalist\n1.also not");
        check(d.blocks.size() == 1 && d.blocks[0].type == BlockType::Paragraph,
              "list: marker requires a trailing space");
    }

    // ---- tables ----
    {
        Document d = parse("| A | B |\n| --- | --- |\n| 1 | 2 |\n| 3 | 4 |");
        check(d.blocks.size() == 1 && d.blocks[0].type == BlockType::Table, "table: parses");
        const Block& t = d.blocks[0];
        check(t.headers.size() == 2 && runText(t.headers[0].runs) == "A"
                  && runText(t.headers[1].runs) == "B",
              "table: header cells");
        check(t.rows.size() == 2 && t.rows[1].cells.size() == 2
                  && runText(t.rows[1].cells[1].runs) == "4",
              "table: body rows and cells");
        check(t.aligns.size() == 2 && t.aligns[0] == AlignLeft && t.aligns[1] == AlignLeft,
              "table: default alignment left");
        check(t.srcStartLine == 0 && t.srcEndLine == 3, "table: source line range");
    }
    {
        Document d = parse("| L | C | R |\n| :--- | :---: | ---: |");
        check(d.blocks.size() == 1 && d.blocks[0].aligns.size() == 3
                  && d.blocks[0].aligns[0] == AlignLeft
                  && d.blocks[0].aligns[1] == AlignCenter
                  && d.blocks[0].aligns[2] == AlignRight,
              "table: :---, :---:, ---: alignments");
    }
    {
        Document d = parse("intro\n\n| A |\n| --- |\n| 1 |\n\noutro");
        check(d.blocks.size() == 3 && d.blocks[1].type == BlockType::Table,
              "table: bounded by paragraphs");
        check(d.blocks[1].srcStartLine == 2 && d.blocks[1].srcEndLine == 4,
              "table: line range offset by preceding blocks");
    }
    {
        Document d = parse("| A | B |\nno separator follows");
        check(!d.blocks.empty() && d.blocks[0].type == BlockType::Paragraph,
              "table: pipe line without separator is a paragraph");
    }
    {
        Document d = parse("| **H** | `c` |\n| --- | --- |\n| [l](u) | x |");
        const Block& t = d.blocks[0];
        check(t.headers.size() == 2 && t.headers[0].runs.size() == 1
                  && t.headers[0].runs[0].bold,
              "table: inline styling in header cells");
        check(t.rows.size() == 1 && !t.rows[0].cells.empty()
                  && !t.rows[0].cells[0].runs.empty()
                  && u8(t.rows[0].cells[0].runs[0].href) == "u",
              "table: links in body cells");
    }
    {
        Document d = parse("| A |\n| --- |\n| one<br>two |");
        const Block& t = d.blocks[0];
        check(t.rows.size() == 1 && runText(t.rows[0].cells[0].runs) == "one\ntwo",
              "table: <br> in a body cell folds to a newline");
    }
    {
        auto cells = SplitTableCells(FromUtf8("| a | b \\| c |"));
        check(cells.size() == 2 && u8(cells[0]) == "a" && u8(cells[1]) == "b \\| c",
              "table: escaped pipe stays inside its cell");
        auto plain = SplitTableCells(FromUtf8("x | y"));
        check(plain.size() == 2 && u8(plain[0]) == "x" && u8(plain[1]) == "y",
              "table: borderless row splits");
    }

    // ---- mixed document / CR handling ----
    {
        Document d = parse("# H\r\npara text\r\n\r\n- item\r\n");
        check(d.blocks.size() == 3, "crlf: CR bytes are tolerated");
        check(d.blocks[0].type == BlockType::Heading
                  && d.blocks[1].type == BlockType::Paragraph
                  && d.blocks[2].type == BlockType::ListItem,
              "crlf: block types unchanged");
    }
    {
        Document d = parse("");
        check(d.blocks.empty(), "empty input yields empty document");
    }

    return summary();
}
