#pragma once
#include "shell.h"
#include "help.h"
#include <charconv>

namespace ccc {

inline bool Shell::applyRedirs(const std::vector<Redir>& rs, Ctx& c, Handles& hs) {
    for (const Redir& r : rs) {
        if (r.dupTo >= 0) {
            if (r.fd == 2 && r.dupTo == 1) c.err = c.out;
            else if (r.fd == 1 && r.dupTo == 2) c.out = c.err;
            continue;
        }
        std::string target = unquote(expandLate(r.target));
        if (ieq(target, "nul") && r.fd != 0) {
            (r.fd == 1 ? c.out : c.err) = Sink{};
            (r.fd == 1 ? c.out : c.err).discard = true;
            continue;
        }
        os::Handle h = r.fd == 0 ? os::openRead(target) : os::openWrite(target, r.append);
        if (h == os::NO_HANDLE) { c.err.line(systemMessage(os::lastError())); errorlevel = 1; return false; }
        hs.h.push_back(h);
        Sink s;
        s.h = h;
        if (r.fd != 0) s.out = hs.bufferFor(h, unicodeOutput);
        if (r.fd == 0) c.in = Source{ h };
        else if (r.fd == 1) c.out = s;
        else if (r.fd == 2) c.err = s;
    }
    return true;
}

inline int Shell::exec(Node* n, Ctx& c) {
    if (!n) return 0;
    checkInterrupt();
    switch (n->kind) {
    case Node::Seq: { int r = exec(n->a.get(), c); return unwinding() ? r : exec(n->b.get(), c); }
    case Node::And: { int r = exec(n->a.get(), c); return r == 0 && !unwinding() ? exec(n->b.get(), c) : r; }
    case Node::Or: { int r = exec(n->a.get(), c); return r != 0 && !unwinding() ? exec(n->b.get(), c) : r; }
    case Node::Pipe: return execPipe(n, c);
    case Node::Block: {
        Handles hs;
        Ctx cc = c;
        if (!applyRedirs(n->redirs, cc, hs)) return 1;
        int r = 0;
        for (auto& x : n->list) {
            r = exec(x.get(), cc);
            if (unwinding()) break;
        }
        return r;
    }
    case Node::If: return execIf(n, c);
    case Node::For: return execFor(n, c);
    default: break;
    }
    std::string text = expandLate(n->text);
    if (n->redirs.empty()) return isBlank(text) ? 0 : command(text, c, n);
    Handles hs;
    Ctx cc = c;
    if (!applyRedirs(n->redirs, cc, hs)) return 1;
    return command(text, cc, n);
}

inline int Shell::execPipe(Node* n, Ctx& c) {
    os::Handle r, w;
    if (!os::createPipe(r, w, os::PIPE_BYTES)) return 1;
    Ctx left = c, right = c;
    right.in = Source{ r };
    pushLocal();
    struct Restore { Shell* s; size_t depth; ~Restore() { while (s->locals.size() >= depth) s->endlocal(); } } restore{ this, locals.size() };
    std::thread t([&] {
        {
            OutputBuffer pipeOut(nullptr, w);
            pipeOut.setUtf16(unicodeOutput);
            left.out.toHandle(w);
            left.out.out = &pipeOut;
            try { exec(n->a.get(), left); } catch (...) {}
        }
        os::close(w);
    });
    int rc = 0;
    try { rc = exec(n->b.get(), right); } catch (...) { os::close(r); t.join(); throw; }
    os::close(r);
    t.join();
    return rc;
}

inline int Shell::execIf(Node* n, Ctx& c) {
    bool r = false;
    std::string a = expandLate(n->ifA);
    if (n->ifKind == "ERRORLEVEL") r = errorlevel >= atoi(a.c_str());
    else if (n->ifKind == "EXIST") r = exists(unquote(a)) || hasMatch(unquote(a));
    else if (n->ifKind == "DEFINED") { std::string v; r = getVar(a, v); }
    else if (n->ifKind == "CMDEXTVERSION") r = 2 >= atoi(a.c_str());
    else {
        std::string b = expandLate(n->ifB);
        if (n->ifOp == "==") r = n->ifI ? ieq(a, b) : a == b;
        else {
            long long x, y;
            int cmp;
            if (isNumber(a, x) && isNumber(b, y)) cmp = x < y ? -1 : x > y ? 1 : 0;
            else cmp = n->ifI ? compareNoCase(unquote(a), unquote(b)) : strcmp(unquote(a).c_str(), unquote(b).c_str());
            const std::string& o = n->ifOp;
            if (o == "EQU") r = cmp == 0; else if (o == "NEQ") r = cmp != 0; else if (o == "LSS") r = cmp < 0;
            else if (o == "LEQ") r = cmp <= 0; else if (o == "GTR") r = cmp > 0; else if (o == "GEQ") r = cmp >= 0;
            else { c.err.line(message(Msg::Unexpected, { n->ifOp })); errorlevel = 255; return 255; }
        }
    }
    if (n->ifNot) r = !r;
    if (r) return exec(n->body.get(), c);
    if (n->els) return exec(n->els.get(), c);
    return 0;
}

inline bool Shell::hasMatch(const std::string& pattern) {
    os::FileInfo found;
    return os::findFile(pattern, found);
}

inline int Shell::runBody(Node* n, Ctx& c, char var, std::string_view value) {
    checkInterrupt();
    forVars.set(var, value);
    if (echoOn && !quietLine(*n->body)) c.out.write("\r\n" + prompt() + expandForVars(echoText(*n->body)) + "\r\n");
    return exec(n->body.get(), c);
}

inline int Shell::execFor(Node* n, Ctx& c) {
    char var = n->forVar[1];
    std::string set = expandLate(n->forSet);
    const std::string& sw = n->forSwitch;
    ForScope scope(forVars, var, sw.find('F') != std::string::npos ? (std::min)(32, 128 - (int)ForVars::slot(var)) : 1);
    int r = 0;
    if (sw.find('L') != std::string::npos) {
        auto p = splitArgs(set);
        long long a = p.size() > 0 ? atoll(p[0].c_str()) : 0, st = p.size() > 1 ? atoll(p[1].c_str()) : 0, e = p.size() > 2 ? atoll(p[2].c_str()) : 0;
        if (st == 0) return 0;
        for (DecimalCounter v(a); (st > 0 ? v.current() <= e : v.current() >= e) && !unwinding(); v.add(st)) r = runBody(n, c, var, v.text());
        return r;
    }
    if (sw.find('F') != std::string::npos) return forF(n, c, var, set);
    bool dirs = sw.find('D') != std::string::npos;
    auto items = splitArgs(set);
    auto each = [&](const std::string& base) {
        for (const std::string& item0 : items) {
            if (unwinding()) return;
            std::string item = unquote(item0);
            if (!wild(item)) { r = runBody(n, c, var, base.empty() ? item0 : base + item); continue; }
            std::string pat = base + item, dir = dirOf(pat);
            for (auto& e : list(pat)) {
                if (e.name == "." || e.name == "..") continue;
                bool isDir = (e.attr & os::ATTR_DIRECTORY) != 0;
                if (isDir != dirs || (e.attr & (os::ATTR_HIDDEN | os::ATTR_SYSTEM))) continue;
                if (unwinding()) return;
                r = runBody(n, c, var, dir + e.name);
            }
        }
    };
    if (sw.find('R') != std::string::npos) {
        std::string root = fullPath(n->forRoot.empty() ? "." : expandLate(n->forRoot));
        std::function<void(const std::string&)> walk = [&](const std::string& d) {
            if (unwinding()) return;
            std::string base = withSlash(d);
            each(base);
            for (auto& e : list(base + "*"))
                if ((e.attr & os::ATTR_DIRECTORY) && e.name != "." && e.name != ".." && !(e.attr & os::ATTR_REPARSE)) walk(base + e.name);
        };
        walk(root);
        return r;
    }
    each("");
    return r;
}

inline int Shell::forF(Node* n, Ctx& c, char var, const std::string& set) {
    std::string eol = ";", delims = " \t";
    int skip = 0;
    bool backq = false;
    std::vector<int> toks{ 1 };
    bool rest = false;
    std::string o = n->forOpts;
    for (size_t i = 0; i < o.size();) {
        while (i < o.size() && o[i] == ' ') ++i;
        auto opt = [&](const char* k) { size_t l = strlen(k); if (o.size() - i >= l && equalNoCase(o.c_str() + i, k, l)) { i += l; return true; } return false; };
        if (opt("eol=")) { eol = i < o.size() ? std::string(1, o[i++]) : ""; }
        else if (opt("skip=")) { skip = atoi(o.c_str() + i); while (i < o.size() && isdigit((unsigned char)o[i])) ++i; }
        else if (opt("usebackq")) backq = true;
        else if (opt("delims=")) { size_t e = o.find(" tokens=", i); size_t e2 = o.find(" skip=", i); size_t e3 = o.find(" eol=", i); size_t e4 = o.find(" usebackq", i);
            size_t end = (std::min)({ e, e2, e3, e4, o.size() }); delims = o.substr(i, end - i); i = end; }
        else if (opt("tokens=")) {
            toks.clear();
            while (i < o.size() && o[i] != ' ') {
                if (o[i] == '*') { rest = true; ++i; continue; }
                int a = atoi(o.c_str() + i);
                while (i < o.size() && isdigit((unsigned char)o[i])) ++i;
                int b = a;
                if (i < o.size() && o[i] == '-') { ++i; b = atoi(o.c_str() + i); while (i < o.size() && isdigit((unsigned char)o[i])) ++i; }
                for (int k = a; k <= b && a > 0; ++k) toks.push_back(k);
                if (i < o.size() && o[i] == ',') ++i;
            }
            std::sort(toks.begin(), toks.end());
        }
        else ++i;
    }
    std::string text, s = trim(set);
    char open = s.empty() ? 0 : s[0];
    if ((!backq && open == '\'') || (backq && open == '`')) {
        Ctx cap = c;
        cap.out = Sink();
        cap.out.buf = &text;
        std::string cmd = s.substr(1, s.size() >= 2 ? s.size() - 2 : 0);
        Parser p(cmd);
        NodeP tree = p.parse();
        exec(tree.get(), cap);
    }
    else if ((!backq && open == '"') || (backq && open == '\'')) text = s.substr(1, s.size() >= 2 ? s.size() - 2 : 0);
    else {
        for (auto& f : splitArgs(s)) {
            std::string path = unquote(f);
            std::string buf;
            if (!readFile(path, buf)) { c.err.line(message(Msg::CannotFindFile, { path })); errorlevel = 1; return 1; }
            text += buf;
        }
    }
    int r = 0, lineNo = 0;
    size_t pos = 0;
    while (pos < text.size() && !unwinding()) {
        size_t e = text.find('\n', pos);
        if (e == std::string::npos) e = text.size();
        std::string line = text.substr(pos, e - pos);
        pos = e + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (lineNo++ < skip || line.empty()) continue;
        if (!eol.empty() && line[0] == eol[0]) continue;
        std::vector<std::pair<size_t, size_t>> spans;
        for (size_t a = line.find_first_not_of(delims); a != std::string::npos; a = line.find_first_not_of(delims, a)) {
            size_t b = (std::min)(line.find_first_of(delims, a), line.size());
            spans.push_back({ a, b });
            a = b;
        }
        char v = var;
        bool any = false;
        for (int k : toks) {
            std::string val = k - 1 < (int)spans.size() ? line.substr(spans[k - 1].first, spans[k - 1].second - spans[k - 1].first) : "";
            forVars.set(v++, val);
            any |= !val.empty();
        }
        if (rest) {
            int last = toks.empty() ? 0 : toks.back();
            forVars.set(v, last < (int)spans.size() ? line.substr(spans[last].first) : "");
            any |= !forVars.get(v).empty();
        }
        if (!any && !toks.empty()) continue;
        r = exec(n->body.get(), c);
    }
    return r;
}

inline bool Shell::hasHelpSwitch(const std::string& text, size_t from) {
    for (const char* p = text.data() + from, *e = text.data() + text.size(); (p = findChar(p, e, '?')) != nullptr; ++p)
        if (p > text.data() && p[-1] == '/') return true;
    return false;
}

inline int Shell::command(const std::string& text, Ctx& c, Node* n) {
    size_t i = 0;
    while (i < text.size() && isDelim(text[i])) ++i;
    if (i >= text.size()) return 0;
    size_t j = i;
    while (j < text.size() && isAsciiAlpha(text[j])) ++j;
    char next = j < text.size() ? text[j] : 0;
    if ((next == 0 || isDelim(next) || next == '/') && hasHelpSwitch(text, j)) {
        auto help = helpTexts().find(upper(text.substr(i, j - i)));
        if (help != helpTexts().end())
            for (auto& a : splitArgs(text.substr(j)))
                if (a == "/?") { c.out.write(crlf(help->second)); return 0; }
    }
    const BuiltinInfo* info = findBuiltin(text.data() + i, j - i);
    if (info && (next == 0 || isDelim(next) || strchr(".:(/\\[]+\"", next))) {
        if (info->fn == &Shell::cmdEcho && next && !isDelim(next)) {
            c.out.line(text.data() + j + 1, text.size() - j - 1);
            return 0;
        }
        std::string args = text.substr(next && isDelim(next) ? j + 1 : j);
        int r = (this->*(info->fn))(args, c);
        if (r) errorlevel = r;
        else if (resetsErrorlevel(info->reset)) errorlevel = 0;
        return r;
    }
    if (text.size() == i + 2 && text[i + 1] == ':' && isalpha((unsigned char)text[i])) return changeDrive(text.substr(i, 2), c);
    return external(text.substr(i), c);
}

inline bool Shell::resetsErrorlevel(Reset reset) {
    if (reset == Reset::Always) return true;
    if (reset == Reset::Never) return false;
    Batch* b = batch();
    return !b || b->isCmd;
}

inline int Shell::changeDrive(const std::string& d, Ctx& c) {
    std::string target = upper(d), saved;
    if (!getVar("=" + target, saved)) saved = target + os::SLASH;
    if (!os::setCurrentDirectory(saved) && !os::setCurrentDirectory(target + os::SLASH)) {
        c.err.line(systemMessage(os::lastError()));
        return 1;
    }
    return 0;
}

}
