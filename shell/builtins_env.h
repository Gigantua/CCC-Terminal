#pragma once
#include "shell.h"

namespace ccc {

struct SetExpr {
    Shell& sh;
    const std::string& s;
    size_t i = 0;
    Msg error{};
    void ws() { while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '"')) ++i; }
    long long varValue(const std::string& n) { std::string v; if (!sh.getVar(n, v)) return 0; return strtoll(v.c_str(), nullptr, 0); }
    void assign(const std::string& n, long long v) { std::string t = std::to_string(v); sh.setVar(n, &t); }
    long long comma() { long long v = assignment(); ws(); while (i < s.size() && s[i] == ',') { ++i; v = assignment(); ws(); } return v; }
    long long assignment() {
        ws();
        size_t save = i;
        std::string name;
        while (i < s.size() && (isalnum((unsigned char)s[i]) || s[i] == '_' || s[i] == '$' || s[i] == '.' || s[i] == '#' || s[i] == '@')) name += s[i++];
        if (!name.empty() && !isdigit((unsigned char)name[0])) {
            ws();
            static const char* ops[] = { "<<=", ">>=", "*=", "/=", "%=", "+=", "-=", "&=", "^=", "|=", "=" };
            for (const char* op : ops) {
                size_t l = strlen(op);
                if (s.compare(i, l, op) == 0 && !(l == 1 && i + 1 < s.size() && s[i + 1] == '=')) {
                    i += l;
                    long long rhs = assignment(), cur = varValue(name), v = rhs;
                    switch (op[0]) {
                    case '*': v = cur * rhs; break;
                    case '/': if (!rhs) { error = Msg::DivideByZero; return 0; } v = cur / rhs; break;
                    case '%': if (!rhs) { error = Msg::DivideByZero; return 0; } v = cur % rhs; break;
                    case '+': v = cur + rhs; break;
                    case '-': v = cur - rhs; break;
                    case '&': v = cur & rhs; break;
                    case '^': v = cur ^ rhs; break;
                    case '|': v = cur | rhs; break;
                    case '<': v = cur << rhs; break;
                    case '>': v = cur >> rhs; break;
                    default: break;
                    }
                    v = (int)v;
                    assign(name, v);
                    return v;
                }
            }
        }
        i = save;
        return bor();
    }
    long long bor() { long long v = bxor(); for (;;) { ws(); if (i < s.size() && s[i] == '|' ) { ++i; v |= bxor(); } else return v; } }
    long long bxor() { long long v = band(); for (;;) { ws(); if (i < s.size() && s[i] == '^') { ++i; v ^= band(); } else return v; } }
    long long band() { long long v = shift(); for (;;) { ws(); if (i < s.size() && s[i] == '&') { ++i; v &= shift(); } else return v; } }
    long long shift() {
        long long v = add();
        for (;;) {
            ws();
            if (s.compare(i, 2, "<<") == 0) { i += 2; v = (int)(v << add()); }
            else if (s.compare(i, 2, ">>") == 0) { i += 2; v = (int)v >> add(); }
            else return v;
        }
    }
    long long add() { long long v = mul(); for (;;) { ws(); if (i < s.size() && s[i] == '+') { ++i; v = (int)(v + mul()); } else if (i < s.size() && s[i] == '-') { ++i; v = (int)(v - mul()); } else return v; } }
    long long mul() {
        long long v = unary();
        for (;;) {
            ws();
            if (i >= s.size()) return v;
            char c = s[i];
            if (c != '*' && c != '/' && c != '%') return v;
            ++i;
            long long r = unary();
            if (c == '*') v = (int)(v * r);
            else if (!r) { error = Msg::DivideByZero; return 0; }
            else v = c == '/' ? v / r : v % r;
        }
    }
    long long unary() {
        ws();
        if (i < s.size() && s[i] == '!') { ++i; return !unary(); }
        if (i < s.size() && s[i] == '~') { ++i; return (int)~unary(); }
        if (i < s.size() && s[i] == '-') { ++i; return (int)-unary(); }
        if (i < s.size() && s[i] == '+') { ++i; return unary(); }
        return primary();
    }
    long long primary() {
        ws();
        if (i >= s.size()) { error = Msg::MissingOperand; return 0; }
        if (s[i] == '(') {
            ++i;
            long long v = comma();
            ws();
            if (i >= s.size() || s[i] != ')') { error = Msg::UnbalancedParenthesis; return 0; }
            ++i;
            return v;
        }
        if (isdigit((unsigned char)s[i])) {
            char* e;
            long long v = strtoll(s.c_str() + i, &e, 0);
            size_t n = e - (s.c_str() + i);
            if (i + n < s.size() && isalnum((unsigned char)s[i + n])) { error = Msg::InvalidNumber; return 0; }
            i += n;
            return (int)v;
        }
        std::string name;
        while (i < s.size() && (isalnum((unsigned char)s[i]) || s[i] == '_' || s[i] == '$' || s[i] == '.' || s[i] == '#' || s[i] == '@')) name += s[i++];
        if (name.empty()) { error = Msg::MissingOperand; return 0; }
        return varValue(name);
    }
};

inline int Shell::assocLike(const std::string& args, Ctx& c, bool ftype) {
    std::string a = trim(args);
    auto keyOf = [&](const std::string& n) { return ftype ? n + "\\shell\\open\\command" : n; };
    size_t eq = a.find('=');
    if (eq != std::string::npos) {
        std::string name = trim(a.substr(0, eq)), value = trim(a.substr(eq + 1));
        if (!os::setClassValue(keyOf(name), value.empty() ? nullptr : &value)) return fail(c);
        if (!value.empty()) c.out.line(name + "=" + value);
        return 0;
    }
    if (!a.empty()) {
        std::string v = os::classValue(keyOf(a));
        if (v.empty()) { c.err.line(message(ftype ? Msg::FtypeNotFound : Msg::AssocNotFound, { a })); return 1; }
        c.out.line(a + "=" + v);
        return 0;
    }
    std::string out;
    for (const std::string& k : os::classKeys()) {
        if ((k[0] == '.') == ftype) continue;
        std::string v = os::classValue(keyOf(k));
        if (!v.empty()) out += k + "=" + v + "\r\n";
    }
    c.out.write(out);
    return 0;
}

inline int Shell::cmdAssoc(const std::string& a, Ctx& c) { return assocLike(a, c, false); }

inline int Shell::cmdFtype(const std::string& a, Ctx& c) { return assocLike(a, c, true); }

inline int Shell::cmdSet(const std::string& args0, Ctx& c) {
    std::string args = trim(args0);
    if (args.size() >= 2 && args[0] == '/' && toupper((unsigned char)args[1]) == 'A') return setA(trim(args.substr(2)), c);
    if (args.size() >= 2 && args[0] == '/' && toupper((unsigned char)args[1]) == 'P') {
        std::string rest = unquote(trim(args.substr(2)));
        size_t eq = rest.find('=');
        if (eq == std::string::npos) return syntaxError(c);
        std::string prompt = rest.substr(eq + 1);
        if (!prompt.empty() && prompt[0] == '"') { size_t q = prompt.rfind('"'); prompt = q > 0 ? prompt.substr(1, q - 1) : prompt.substr(1); }
        size_t lead = prompt.find_first_not_of(" \t");
        c.out.write(lead == std::string::npos ? std::string() : prompt.substr(lead));
        std::string line;
        if (!readInput(line, c) || line.empty() || eq == 0) return 1;
        std::string name = rest.substr(0, eq);
        setVar(name, &line);
        return 0;
    }
    if (!args.empty() && args[0] == '"') {
        size_t close = args.rfind('"');
        args = close > 0 ? args.substr(1, close - 1) : args.substr(1);
    }
    size_t eq = args.find('=');
    if (eq == std::string::npos || eq == 0) {
        std::string prefix = upper(args), out;
        this->env.sync();
        for (const std::string& e : os::environment()) {
            if (e[0] == '=') continue;
            if (prefix.empty() || upper(e.substr(0, prefix.size())) == prefix) out += e + "\r\n";
        }
        if (out.empty()) { c.err.line(message(Msg::VariableNotDefined, { args })); return 1; }
        c.out.write(out);
        return 0;
    }
    std::string name = args.substr(0, eq), value = args.substr(eq + 1);
    setVar(name, value.empty() ? nullptr : &value);
    return 0;
}

inline int Shell::setA(const std::string& e, Ctx& c) {
    if (e.empty()) return syntaxError(c);
    SetExpr x{ *this, e };
    long long v = x.comma();
    x.ws();
    if (x.error == Msg{} && x.i < e.size()) x.error = Msg::MissingOperator;
    if (x.error != Msg{}) { c.err.line(message(x.error)); return (int)x.error; }
    if (!batch()) c.out.write(std::to_string((int)v));
    return 0;
}

inline void Shell::pushLocal() {
    Local l;
    l.delayed = delayed;
    l.extensions = extensions;
    l.cwd = cwd();
    this->env.sync();
    for (const std::string& e : os::environment()) {
        size_t eq = e.find('=', 1);
        if (eq != std::string::npos) l.env.push_back({ e.substr(0, eq), e.substr(eq + 1) });
    }
    locals.push_back(std::move(l));
}

inline int Shell::cmdSetlocal(const std::string& args, Ctx&) {
    pushLocal();
    if (batch()) ++batch()->localDepth;
    for (auto& a : splitArgs(args)) {
        std::string u = upper(a);
        if (u == "ENABLEDELAYEDEXPANSION") delayed = true;
        else if (u == "DISABLEDELAYEDEXPANSION") delayed = false;
        else if (u == "ENABLEEXTENSIONS") extensions = true;
        else if (u == "DISABLEEXTENSIONS") extensions = false;
        else { stderrSink().line(message(Msg::SetlocalBadParameter)); errorlevel = 1; }
    }
    return 0;
}

inline void Shell::endlocal() {
    if (locals.empty()) return;
    Local l = std::move(locals.back());
    locals.pop_back();
    struct NoCase { bool operator()(const std::string& a, const std::string& b) const { return compareNoCase(a, b) < 0; } };
    std::map<std::string, std::string, NoCase> current;
    for (const std::string& e : os::environment()) {
        size_t eq = e.find('=', 1);
        if (eq != std::string::npos) current.emplace(e.substr(0, eq), e.substr(eq + 1));
    }
    for (auto& kv : l.env) {
        auto it = current.find(kv.first);
        if (it != current.end() && it->first != kv.first) os::setEnvironment(it->first, nullptr);
        if (it == current.end() || it->second != kv.second || it->first != kv.first) os::setEnvironment(kv.first, &kv.second);
        if (it != current.end()) current.erase(it);
    }
    for (auto& kv : current) os::setEnvironment(kv.first, nullptr);
    this->env.reload();
    delayed = l.delayed;
    extensions = l.extensions;
    os::setCurrentDirectory(l.cwd);
}

inline int Shell::cmdEndlocal(const std::string&, Ctx&) {
    if (batch() && batch()->localDepth > 0) { --batch()->localDepth; endlocal(); }
    else if (!batch()) endlocal();
    return 0;
}

inline int Shell::pathLike(const std::string& var, const std::string& args0, Ctx& c) {
    std::string args = trim(args0);
    if (!args.empty() && args[0] == '=') args = trim(args.substr(1));
    if (args.empty()) {
        std::string v;
        if (getVar(var, v)) c.out.line(var + "=" + v);
        else c.out.line(var + "=(null)");
        return 0;
    }
    if (args == ";") { setVar(var, nullptr); return 0; }
    setVar(var, &args);
    return 0;
}

inline int Shell::cmdPath(const std::string& a, Ctx& c) { return pathLike("PATH", a, c); }

inline int Shell::cmdDpath(const std::string& a, Ctx& c) { return pathLike("DPATH", a, c); }

inline int Shell::cmdPrompt(const std::string& a, Ctx&) {
    std::string t = trim(a);
    if (t.empty()) setVar("PROMPT", nullptr);
    else setVar("PROMPT", &t);
    promptText.clear();
    return 0;
}

}
