// Shared rich-clipboard (HTML) fragment builder — see html_copy.h.
//
// This is the single source of the copy-to-clipboard HTML markup: it was the
// macOS `copySelection:` algorithm (issue #36, hardened by 80aaa22), lifted
// into the core so the Windows frontend copies byte-identical markup and the
// rules have direct unit tests (tests/html_copy_test.cpp).
#include "html_copy.h"

#include <cmath>

namespace fmdv {

namespace {

// Never split a UTF-16 surrogate pair in a slice: a lone surrogate encodes to
// invalid UTF-8, which makes NSString's UTF-8 initializers return nil (macOS)
// or MultiByteToWideChar fail (Windows).
bool splitsUtf16Pair(const Str& t, int idx) {
    return idx > 0 && idx < (int)t.size() &&
           t[idx - 1] >= 0xD800 && t[idx - 1] <= 0xDBFF &&
           t[idx] >= 0xDC00 && t[idx] <= 0xDFFF;
}

// Escaping shared by text content and (quoted) attribute values. '&' first,
// so the entity introducers of the later substitutions are never re-escaped.
std::string htmlEscape(const std::string& utf8) {
    std::string out;
    out.reserve(utf8.size());
    for (unsigned char c : utf8) {
        if (c == '&') out += "&amp;";
        else if (c == '<') out += "&lt;";
        else if (c == '>') out += "&gt;";
        else if (c == '"') out += "&quot;";
        else out += (char)c;
    }
    return out;
}

const char* headingTag(FontRole r) {
    switch (r) {
        case FontRole::H1: return "h1";
        case FontRole::H2: return "h2";
        case FontRole::H3: return "h3";
        case FontRole::H4: return "h4";
        case FontRole::H5: return "h5";
        case FontRole::H6: return "h6";
        default: return nullptr;
    }
}

}  // namespace

std::string ClipboardHtmlFragment(const std::vector<CopyFrag>& frags,
                                  std::size_t start, std::size_t startCh,
                                  std::size_t end, std::size_t endCh,
                                  const std::vector<LinkHit>& links) {
    if (frags.empty() || start > end || start >= frags.size() || end >= frags.size())
        return "";

    bool inLink = false;
    std::string currentHref;
    // A heading can wrap onto more than one display line, each its own frag;
    // track the open <hN> across those fragments instead of re-emitting it
    // per fragment (which produced one sibling heading per line).
    bool inHeading = false;
    FontRole currentHeadingRole = FontRole::Body;

    std::string out;
    for (std::size_t i = start; i <= end; i++) {
        const CopyFrag& f = frags[i];
        Str fragStr = f.text;
        int c0 = (i == start) ? (int)startCh : 0;
        int c1 = (i == end) ? (int)endCh : (int)fragStr.size();
        if (splitsUtf16Pair(fragStr, c0)) c0--;
        if (splitsUtf16Pair(fragStr, c1)) c1++;
        if (c1 < c0) continue;
        if (c0 < 0) c0 = 0;
        if (c1 > (int)fragStr.size()) c1 = (int)fragStr.size();
        std::string sub = ToUtf8(fragStr.substr(c0, (size_t)(c1 - c0)));

        // Check for overlapping links in this fragment range (first hit wins,
        // matching the click hit-test ordering).
        std::string hrefForFrag;
        for (const auto& lk : links) {
            if (lk.rect.y + lk.rect.h > f.box.y && lk.rect.y < f.box.y + f.box.h &&
                lk.rect.x + lk.rect.w > f.box.x && lk.rect.x < f.box.x + f.box.w) {
                hrefForFrag = ToUtf8(lk.href);
                break;
            }
        }

        bool closeLink = inLink && (currentHref.empty() || currentHref != hrefForFrag);
        bool closeHeading = inHeading && f.font.role != currentHeadingRole;
        // Nesting: heading (block, outermost) wraps link (inline); close
        // innermost (link) first, then the heading it was nested in.
        if (closeLink) {
            out += "</a>";
            inLink = false;
            currentHref.clear();
        }
        if (closeHeading) {
            out += "</" + std::string(headingTag(currentHeadingRole)) + ">";
            inHeading = false;
        }

        // A break is inserted between fragments on different lines and a
        // space between fragments separated by a horizontal gap — matches
        // the plain-text copy's spacing so the two copies stay in sync,
        // instead of running adjacent fragments' text together.
        if (i > start) {
            const CopyFrag& prev = frags[i - 1];
            if (std::fabs(f.baseline - prev.baseline) > 1)
                out += "<br>";
            else if (f.box.x - (prev.box.x + prev.box.w) > 2)
                out += " ";
        }

        const char* fragHeadingTag = headingTag(f.font.role);
        bool openHeading = fragHeadingTag && !inHeading;
        bool openLink = !hrefForFrag.empty() && hrefForFrag != currentHref;
        if (openHeading) {
            out += "<" + std::string(fragHeadingTag) + ">";
            inHeading = true;
            currentHeadingRole = f.font.role;
        }
        if (openLink) {
            out += "<a href=\"";
            out += htmlEscape(hrefForFrag);
            out += "\">";
            inLink = true;
            currentHref = hrefForFrag;
        }

        std::string openTags, closeTags;
        // white-space:pre keeps a code line's literal spacing/indentation;
        // line breaks between code lines come from the baseline check above.
        if (f.font.role == FontRole::Mono) {
            openTags += "<code style=\"white-space:pre\">";
            closeTags = "</code>" + closeTags;
        }
        if (f.font.bold) { openTags += "<b>"; closeTags = "</b>" + closeTags; }
        if (f.font.italic) { openTags += "<i>"; closeTags = "</i>" + closeTags; }

        out += openTags;
        out += htmlEscape(sub);
        out += closeTags;
    }

    if (inLink) {
        out += "</a>";
        inLink = false;
        currentHref.clear();
    }
    if (inHeading) {
        out += "</" + std::string(headingTag(currentHeadingRole)) + ">";
        inHeading = false;
    }
    return out;
}

}  // namespace fmdv
