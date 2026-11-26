#pragma once
#include "shell.h"

namespace ccc {

inline int Shell::cmdGoto(const std::string& args, Ctx& c) {
    Batch* b = batch();
    std::string label = trim(args);
    if (!label.empty() && label[0] == ':') label = label.substr(1);
    auto words = splitArgs(label);
    label = words.empty() ? "" : words[0];
    if (label.empty()) { c.err.line(message(Msg::GotoNoLabel)); return b ? exitBatch(1) : 1; }
    if (!b) return 0;
    if (ieq(label, "EOF")) return exitBatch(errorlevel);
    size_t k = findLabel(*b, label);
    if (k != std::string::npos) {
        b->pc = k + 1;
        unwind = Unwind::Goto;
        return 0;
    }
    c.err.line(message(Msg::LabelNotFound, { label }));
    return exitBatch(1);
}

inline int Shell::cmdCall(const std::string& args0, Ctx& c) {
    std::string args = trim(args0);
    if (args.empty()) return syntaxError(c);
    Batch* b = batch();
    if (args[0] == ':') {
        if (!b) { c.err.line(message(Msg::CallLabelOutsideBatch)); return 1; }
        auto a = splitArgs(args);
        Batch sub = *b;
        sub.args = a;
        sub.tail = argTail(args.substr(a[0].size()));
        sub.ctx = &c;
        sub.localDepth = 0;
        std::string label = a[0].substr(1);
        size_t k = findLabel(*b, label);
        if (k == std::string::npos) { c.err.line(message(Msg::LabelNotFound, { label })); return 1; }
        sub.pc = k + 1;
        return runBatch(sub);
    }
    std::string expanded = expandPercent(args, b);
    Parser p(expanded);
    NodeP n = p.parse();
    if (n && n->kind == Node::Cmd) {
        std::string text = expandLate(n->text);
        size_t w = 0;
        while (w < text.size() && isAsciiAlpha(text[w])) ++w;
        char next = w < text.size() ? text[w] : 0;
        if (w && findBuiltin(text.data(), w) && (next == 0 || isDelim(next) || strchr(".:(/\\[]+\"", next))) return exec(n.get(), c);
        size_t e = tokenEnd(text);
        std::string path = resolve(text.substr(0, e));
        if (isBatchExtension(upperExtension(path))) {
            auto a = splitArgs(text.substr(e));
            a.insert(a.begin(), text.substr(0, e));
            return runBatchFile(path, a, argTail(text.substr(e)), &c);
        }
    }
    return exec(n.get(), c);
}

inline int Shell::cmdShift(const std::string& args, Ctx&) {
    Batch* b = batch();
    if (!b) return 0;
    std::string a = trim(args);
    size_t from = a.size() == 2 && a[0] == '/' && isdigit((unsigned char)a[1]) ? a[1] - '0' : 0;
    if (from < b->args.size()) b->args.erase(b->args.begin() + from);
    return 0;
}

inline int Shell::cmdExit(const std::string& args, Ctx&) {
    Opts o = opts(args, "B");
    int code = o.pos.empty() ? errorlevel : atoi(o.pos[0].c_str());
    if (o.has('B') && batch()) { errorlevel = code; return exitBatch(code); }
    if (o.has('B')) { errorlevel = code; return code; }
    throw ExitShell{ code };
}

inline int Shell::cmdEcho(const std::string& args, Ctx& c) {
    size_t a = args.find_first_not_of(" \t");
    if (a == std::string::npos) { c.out.line(message(echoOn ? Msg::EchoOn : Msg::EchoOff)); return 0; }
    std::string_view t = std::string_view(args).substr(a, args.find_last_not_of(" \t") + 1 - a);
    if (ieq(t, "on")) { echoOn = true; return 0; }
    if (ieq(t, "off")) { echoOn = false; return 0; }
    c.out.line(args.data(), args.size());
    return 0;
}

inline int Shell::cmdRem(const std::string&, Ctx&) { return 0; }

inline int Shell::cmdPause(const std::string&, Ctx& c) {
    c.out.write(message(Msg::PressAnyKey));
    if (c.in.hasHandle()) { std::string l; readInput(l, c); }
    else { OutputBuffer::flushAll(); host.waitKey(); }
    c.out.write("\r\n");
    return 0;
}

}
