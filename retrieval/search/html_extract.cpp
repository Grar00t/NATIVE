#include "html_extract.hpp"

#include <string_view>
#include <utility>

namespace niyah::search {
namespace {

using View = std::string_view;
constexpr std::size_t link_limit = 2048;

bool space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

char lower(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
}

bool equal_name(View a, View b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (lower(a[i]) != b[i]) return false;
    return true;
}

bool letter(char c) { return lower(c) >= 'a' && lower(c) <= 'z'; }

char next_char(View text, std::size_t& pos) {
    if (text[pos] == '&') {
        struct Entity { View name; char value; };
        static constexpr Entity entities[] = {
            {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&#39;", '\''}
        };
        for (const auto& e : entities) {
            if (text.compare(pos, e.name.size(), e.name) == 0) {
                pos += e.name.size();
                return e.value;
            }
        }
    }
    return text[pos++];
}

void append_text(std::string& out, bool& gap, View fragment) {
    for (std::size_t i = 0; i < fragment.size();) {
        char c = next_char(fragment, i);
        if (space(c)) { gap = true; continue; }
        if (gap && !out.empty()) out.push_back(' ');
        out.push_back(c);
        gap = false;
    }
}

// A '>' inside a quoted attribute belongs to its value.
std::size_t tag_end(View html, std::size_t pos) {
    char quote = 0;
    for (; pos < html.size(); ++pos) {
        const char c = html[pos];
        if (quote) {
            if (c == quote) quote = 0;
        } else if (c == '"' || c == '\'') {
            quote = c;
        } else if (c == '>') {
            return pos;
        }
    }
    return View::npos;
}

void read_links(View attrs, std::vector<std::string>& links) {
    for (std::size_t i = 0; i < attrs.size() && links.size() < link_limit;) {
        if (space(attrs[i]) || attrs[i] == '/' || attrs[i] == '=') { ++i; continue; }
        const auto start = i;
        while (i < attrs.size() && !space(attrs[i]) && attrs[i] != '=' && attrs[i] != '/') ++i;
        const auto name = attrs.substr(start, i - start);
        while (i < attrs.size() && space(attrs[i])) ++i;
        if (i == attrs.size() || attrs[i] != '=') continue;
        do { ++i; } while (i < attrs.size() && space(attrs[i]));
        if (i == attrs.size()) break;
        char quote = attrs[i] == '\'' || attrs[i] == '"' ? attrs[i++] : 0;
        const auto first = i;
        while (i < attrs.size() && (quote ? attrs[i] != quote : !space(attrs[i]))) ++i;
        const auto value = attrs.substr(first, i - first);
        if (quote && i < attrs.size()) ++i;
        if (!equal_name(name, "href") || value.empty() || value.front() == '#') continue;
        std::string href;
        for (std::size_t p = 0; p < value.size();) href.push_back(next_char(value, p));
        links.push_back(std::move(href));
    }
}

std::size_t after_raw_text(View html, std::size_t pos, View name) {
    while ((pos = html.find("</", pos)) != View::npos) {
        pos += 2;
        const auto end = pos + name.size();
        if (end >= html.size() || !equal_name(html.substr(pos, name.size()), name) ||
            (!space(html[end]) && html[end] != '>' && html[end] != '/')) continue;
        const auto close = tag_end(html, end);
        return close == View::npos ? html.size() : close + 1;
    }
    return html.size();
}

} // namespace

HtmlDocument extract_html(const std::string& source, const std::string&) {
    const View html(source);
    HtmlDocument doc;
    bool gap = false, title_gap = false, in_title = false, title_seen = false;
    const auto append = [&](View text) {
        append_text(doc.text, gap, text);
        if (in_title) append_text(doc.title, title_gap, text);
    };
    for (std::size_t pos = 0; pos < html.size();) {
        const auto open = html.find('<', pos);
        append(html.substr(pos, open == View::npos ? View::npos : open - pos));
        if (open == View::npos) break;
        if (html.compare(open, 4, "<!--") == 0) {
            const auto close = html.find("-->", open + 4);
            if (close == View::npos) break;
            pos = close + 3;
            gap = title_gap = true;
            continue;
        }
        pos = open + 1;
        const bool closing = pos < html.size() && html[pos] == '/';
        const auto first = pos + (closing ? 1u : 0u);
        const bool declaration = pos < html.size() && (html[pos] == '!' || html[pos] == '?');
        if (!declaration && (first >= html.size() || !letter(html[first]))) {
            append("<");
            continue;
        }
        std::size_t end = first;
        while (end < html.size() && !space(html[end]) && html[end] != '/' && html[end] != '>') ++end;
        const auto close = tag_end(html, end);
        if (close == View::npos) break;
        pos = close + 1;
        gap = title_gap = true;
        if (declaration) continue;
        const auto name = html.substr(first, end - first);
        if (equal_name(name, "title")) {
            if (closing) in_title = false;
            else if (!title_seen) { title_seen = true; in_title = true; }
        }
        if (closing) continue;
        read_links(html.substr(end, close - end), doc.links);
        if (equal_name(name, "script")) pos = after_raw_text(html, pos, "script");
        else if (equal_name(name, "style")) pos = after_raw_text(html, pos, "style");
    }
    return doc;
}

} // namespace niyah::search
