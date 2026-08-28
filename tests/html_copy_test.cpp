// Unit tests for the shared rich-clipboard HTML builder (core/html_copy) —
// the single source of the copy-to-clipboard markup for BOTH frontends
// (issue #36, hardened by 80aaa22). These pin the exact tag streams:
// nesting order, heading tracking across wrapped fragments, link continuity
// vs adjacent-link splitting, escaping, surrogate-pair-safe slices, and the
// <br>/space separators that keep rich and plain copies in sync.
#include "html_copy.h"
#include "test_check.h"

#include <string>
#include <vector>

using namespace fmdv;

static std::string build(const std::vector<CopyFrag>& frags,
                         std::size_t a, int aCh, std::size_t b, int bCh,
                         const std::vector<LinkHit>& links = {}) {
    return ClipboardHtmlFragment(frags, a, aCh, b, bCh, links);
}

static CopyFrag frag(const char* text, FontSpec font = FontSpec{},
                     double x = 0, double y = 0, double w = 50, double h = 18,
                     double baseline = 0) {
    CopyFrag f;
    f.box = {x, y, w, h};
    f.font = font;
    f.text = FromUtf8(text);
    f.baseline = baseline;
    return f;
}

static FontSpec bold()   { FontSpec f; f.bold = true;   return f; }
static FontSpec italic() { FontSpec f; f.italic = true; return f; }
static FontSpec mono()   { FontSpec f; f.role = FontRole::Mono; return f; }
static FontSpec heading(int n) {
    FontSpec f;
    f.role = static_cast<FontRole>(n);  // H1=2 .. H6=7
    return f;
}

static LinkHit link(double x, double y, double w, double h, const char* href) {
    LinkHit l;
    l.rect = {x, y, w, h};
    l.href = FromUtf8(href);
    return l;
}

int main() {
    // ---- basic runs ----
    check(build({frag("hello")}, 0, 0, 0, 5) == "hello",
          "plain: no tags around plain text");
    check(build({frag("hello world")}, 0, 6, 0, 11) == "world",
          "plain: character slice of one frag");
    check(build({frag("x", bold())}, 0, 0, 0, 1) == "<b>x</b>",
          "style: bold");
    check(build({frag("x", italic())}, 0, 0, 0, 1) == "<i>x</i>",
          "style: italic");
    {
        FontSpec f = bold(); f.italic = true;
        check(build({frag("hi", f)}, 0, 0, 0, 2) == "<b><i>hi</i></b>",
              "style: bold+italic open outer->inner, close inner->outer");
    }
    {
        const std::string codeTag = "<code style=\"white-space:pre\">x</code>";
        check(build({frag("x", mono())}, 0, 0, 0, 1) == codeTag,
              "style: inline code keeps literal whitespace");
    }
    check(build({frag("One", heading(2))}, 0, 0, 0, 3) == "<h1>One</h1>",
          "style: heading level 1 tag");
    check(build({frag("One", heading(7))}, 0, 0, 0, 3) == "<h6>One</h6>",
          "style: heading level 6 tag");

    // ---- separators (keep rich copy in sync with plain-text copy) ----
    {
        std::vector<CopyFrag> f = {
            frag("a", {}, 0, 10, 10, 18, 20),
            frag("b", {}, 20, 10, 10, 18, 20),   // same line, 10px horizontal gap
        };
        check(build(f, 0, 0, 1, 1) == "a b",
              "separator: horizontal gap becomes a space");
    }
    {
        std::vector<CopyFrag> f = {
            frag("a", {}, 0, 10, 40, 18, 20),
            frag("b", {}, 0, 34, 40, 18, 44),   // different line (baselines 20 vs 44)
        };
        check(build(f, 0, 0, 1, 1) == "a<br>b",
              "separator: different baselines become a <br>");
    }
    {
        std::vector<CopyFrag> f = {
            frag("ab", bold(), 0, 10, 20, 18, 20),
            frag("cd", {}, 25, 10, 20, 18, 20),   // 5px horizontal gap
        };
        check(build(f, 0, 0, 1, 2) == "<b>ab</b> cd",
              "separator: styles close before the gap space");
    }

    // ---- headings spanning wrapped fragments ----
    {
        std::vector<CopyFrag> f = {
            frag("line one", heading(3), 0, 10, 90, 30, 38),
            frag("line two", heading(3), 0, 48, 90, 30, 74),
        };
        check(build(f, 0, 0, 1, f[1].text.size()) == "<h2>line one<br>line two</h2>",
              "heading: one open/close pair across wrapped fragments");
    }
    {
        std::vector<CopyFrag> f = {
            frag("H", heading(3), 0, 10, 10, 30, 38),
            frag("body", {}, 0, 50, 30, 18, 64),
        };
        check(build(f, 0, 0, 1, f[1].text.size()) == "<h2>H</h2><br>body",
              "heading: closes when the next fragment is not a heading");
    }

    // ---- links ----
    {
        std::vector<LinkHit> links = {link(0, 10, 30, 18, "https://e.com")};
        check(build({frag("t", {}, 0, 10, 30, 18, 28)}, 0, 0, 0, 1, links)
              == "<a href=\"https://e.com\">t</a>",
              "link: fragment wrapped in <a href>");
    }
    {
        std::vector<CopyFrag> f = {
            frag("ab", {}, 0, 10, 20, 18, 28),
            frag("cd", {}, 20, 10, 20, 18, 28),  // adjacent, same box line
        };
        // one link covering both fragments -> one <a> pair, no re-open
        std::vector<LinkHit> links = {link(0, 10, 40, 18, "https://e.com")};
        check(build(f, 0, 0, 1, 2, links) == "<a href=\"https://e.com\">abcd</a>",
              "link: consecutive fragments share one <a> for one href");
        // two adjacent links with DIFFERENT hrefs must each keep their href —
        // the regression of 80aaa22 (a run merge relabeled [b] under [a]'s href)
        std::vector<LinkHit> two = {
            link(0, 10, 20, 18, "https://a.example/1"),
            link(20, 10, 20, 18, "https://b.example/2"),
        };
        check(build(f, 0, 0, 1, 2, two)
              == "<a href=\"https://a.example/1\">ab</a><a href=\"https://b.example/2\">cd</a>",
              "link: adjacent links keep separate hrefs (close + re-open)");
    }
    {
        std::vector<CopyFrag> f = {
            frag("a", {}, 0, 10, 20, 18, 28),
            frag("b", {}, 25, 10, 20, 18, 28),
        };
        std::vector<LinkHit> links = {
            link(0, 10, 20, 18, "https://a.example/1"),
            link(25, 10, 20, 18, "https://a.example/1"),
        };
        check(build(f, 0, 0, 1, 1, links)
              == "<a href=\"https://a.example/1\">a b</a>",
              "link: same href across a gap stays one <a>");
    }
    {
        std::vector<LinkHit> links = {link(100, 100, 40, 18, "https://far.away")};
        check(build({frag("t", {}, 0, 10, 30, 18, 28)}, 0, 0, 0, 1, links) == "t",
              "link: non-overlapping hit leaves text untagged");
    }
    {
        // link nested in a heading, then a plain body fragment: close the
        // innermost (link) first, then the heading it was nested in
        std::vector<CopyFrag> f = {
            frag("a", heading(2), 0, 10, 30, 30, 38),
            frag("b", {}, 0, 50, 30, 18, 64),
        };
        std::vector<LinkHit> links = {link(0, 10, 30, 30, "https://e.com")};
        check(build(f, 0, 0, 1, 1, links)
              == "<h1><a href=\"https://e.com\">a</a></h1><br>b",
              "link: nesting closes link before heading, then separator");
    }

    // ---- escaping ----
    {
        check(build({frag("a & b <c>")}, 0, 0, 0, 9)
              == "a &amp; b &lt;c&gt;",
              "escape: content & < >");
        check(build({frag("say \"hi\"")}, 0, 0, 0, 9) == "say &quot;hi&quot;",
              "escape: content quotes");
    }
    {
        std::vector<LinkHit> links = {
            link(0, 10, 30, 18, "https://e.com/?a=1&b=2\"x\"")};
        std::string got =
            build({frag("t", {}, 0, 10, 30, 18, 28)}, 0, 0, 0, 1, links);
        check(got == "<a href=\"https://e.com/?a=1&amp;b=2&quot;x&quot;\">t</a>",
              "escape: href attribute entities (& and \")");
    }

    // ---- surrogate-pair-safe slicing (80aaa22 companion hardening) ----
    // "\xF0\x9F\x8E\x89" is U+1F389 (a single unit pair in UTF-16: D83C DF89)
    {
        std::vector<CopyFrag> f = { frag("ab\xF0\x9F\x8E\x89" "cd") };
        // text is a b 🎉 c d -> 6 UTF-16 units: [a][b][D83C][DF89][c][d]
        f[0].text = FromUtf8("ab\xF0\x9F\x8E\x89" "cd");
        check((int)f[0].text.size() == 6, "surrogate: text is 6 UTF-16 units");

        check(build(f, 0, 0, 0, 3) == "ab\xF0\x9F\x8E\x89",
              "surrogate: slice ending on low unit pulls the high unit in");
        check(build(f, 0, 3, 0, 6) == "\xF0\x9F\x8E\x89" "cd",
              "surrogate: slice starting on low unit pulls the high unit in");
        check(build(f, 0, 2, 0, 4) == "\xF0\x9F\x8E\x89",
              "surrogate: slice exactly the pair keeps both halves");
    }

    // ---- degenerate / defensive cases ----
    check(build({frag("hello")}, 0, 3, 0, 3) == "",
          "empty: zero-length selection is the empty fragment");
    check(build({}, 0, 0, 0, 0) == "", "empty: no fragments -> empty");
    check(build({frag("a")}, 0, 0, 1, 0) == "", "empty: out-of-range end -> empty");
    check(build({frag("a")}, 1, 0, 0, 0) == "", "empty: inverted range -> empty");

    return summary();
}
