#include "html_extract.hpp"

#include <iostream>

int main() {
    using niyah::search::extract_html;
    auto doc = extract_html("<!doctype html><TITLE lang='ar'> البحث &amp; النص </title>"
        "<!-- <a href='hidden'> --> <script>href='hidden'</sCriPt ><style>a{}</style>"
        "<p title='a>b'>نص <a data-href='ignore' HREF='/a?x=1&amp;y=2'>مفيد</a></p>"
        "<a href=#part>هنا</a> 3 < 4 &amp;lt;", "");
    if (doc.title != "البحث & النص" || doc.text != "البحث & النص نص مفيد هنا 3 < 4 &lt;" ||
        doc.links != std::vector<std::string>{"/a?x=1&y=2"}) {
        std::cerr << doc.title << '\n' << doc.text << '\n';
        return 1;
    }
    std::string page = "<script>" + std::string(256 * 1024, 'x') + "</script>visible";
    for (int i = 0; i < 2050; ++i) page += "<a href=/index></a>";
    doc = extract_html(page, "");
    if (doc.text != "visible" || doc.links.size() != 2048) return 1;
    return extract_html(" a <style> unfinished", "").text == "a" ? 0 : 1;
}
