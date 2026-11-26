#pragma once
#include <algorithm>
#include <string>
#include <vector>
#include "../shell/util.h"
#include "../shell/unicode.h"

struct LineEdit {
    std::string text;
    size_t pos = 0;

    void set(const std::string& s) { text = s; pos = s.size(); }
    void clear() { text.clear(); pos = 0; }
    void insert(const std::string& s) { text.insert(pos, s); pos += s.size(); }

    void type(const std::string& s, bool insertMode) {
        if (!insertMode && pos < text.size()) text.replace(pos, unicode::utf8Next(text, pos) - pos, s);
        else text.insert(pos, s);
        pos += s.size();
    }

    void backspace() {
        if (!pos) return;
        size_t p = unicode::utf8Prev(text, pos);
        text.erase(p, pos - p);
        pos = p;
    }

    void del() { if (pos < text.size()) text.erase(pos, unicode::utf8Next(text, pos) - pos); }

    size_t wordStart() const {
        size_t b = pos;
        while (b > 0 && text[b - 1] == ' ') --b;
        while (b > 0 && text[b - 1] != ' ') --b;
        return b;
    }

    void wordRight() {
        while (pos < text.size() && text[pos] != ' ') ++pos;
        while (pos < text.size() && text[pos] == ' ') ++pos;
    }

    void deleteWord(bool forward) {
        if (forward) {
            size_t e = pos;
            while (e < text.size() && text[e] == ' ') ++e;
            while (e < text.size() && text[e] != ' ') ++e;
            text.erase(pos, e - pos);
            return;
        }
        size_t b = wordStart();
        text.erase(b, pos - b);
        pos = b;
    }

    void copyFrom(const std::string& last, size_t until) {
        if (pos >= last.size()) return;
        std::string part = last.substr(pos, (std::min)(until, last.size()) - pos);
        text.replace(pos, (std::min)(part.size(), text.size() - pos), part);
        pos += part.size();
    }

    void deleteUpTo(char ch) {
        size_t at = text.find(ch, pos + 1);
        if (at != std::string::npos) text.erase(pos, at - pos);
    }

    bool completing = false;

    void complete(bool backwards) {
        if (!completing) {
            size_t start = 0;
            bool q = false;
            for (size_t k = 0; k < pos; ++k) { if (text[k] == '"') q = !q; else if (!q && text[k] == ' ') start = k + 1; }
            std::string raw = ccc::unquote(text.substr(start, pos - start));
            size_t slash = raw.find_last_of("\\/:");
            std::string dir = slash == std::string::npos ? "" : raw.substr(0, slash + 1);
            matches.clear();
            for (const os::FileInfo& f : os::listFiles(raw + "*"))
                if (f.name != "." && f.name != "..") matches.push_back(dir + f.name);
            if (matches.empty()) return;
            auto lower = [](char c) { return std::tolower((unsigned char)c); };
            std::sort(matches.begin(), matches.end(), [&](const std::string& a, const std::string& b) {
                return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(), [&](char x, char y) { return lower(x) < lower(y); });
            });
            matchStart = start;
            matchEnd = pos;
            match = backwards ? matches.size() - 1 : 0;
            completing = true;
        }
        else match = (match + (backwards ? matches.size() - 1 : 1)) % matches.size();
        std::string m = matches[match];
        if (m.find(' ') != std::string::npos) m = "\"" + m + "\"";
        text.replace(matchStart, matchEnd - matchStart, m);
        matchEnd = matchStart + m.size();
        pos = matchEnd;
    }

private:
    std::vector<std::string> matches;
    size_t match = 0, matchStart = 0, matchEnd = 0;
};
