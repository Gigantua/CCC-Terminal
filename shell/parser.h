#pragma once
#include <functional>
#include <memory>
#include "util.h"

namespace ccc {

struct Redir {
    int fd = 1;
    bool append = false;
    int dupTo = -1;
    std::string target;
};
struct Node {
    enum Kind { Cmd, Seq, And, Or, Pipe, Block, If, For } kind = Cmd;
    std::string text;
    std::vector<Redir> redirs;
    std::unique_ptr<Node> a, b;
    std::vector<std::unique_ptr<Node>> list;
    bool ifNot = false, ifI = false, quiet = false;
    std::string ifKind, ifA, ifOp, ifB;
    std::string forSwitch, forRoot, forOpts, forVar, forSet;
    std::unique_ptr<Node> body, els;
};
using NodeP = std::unique_ptr<Node>;

struct SyntaxError {};

[[noreturn]] inline void syntaxError() { throw SyntaxError{}; }

struct Incomplete {};

class Parser {
public:
    explicit Parser(std::string& s, std::function<bool()> pull = nullptr) : s(s), pull(std::move(pull)) {}
    NodeP parse() {
        NodeP n = seq();
        skipWs();
        if (i < s.size() && s[i] == ')') ++i;
        return n;
    }

private:
    std::string& s;
    std::function<bool()> pull;
    size_t i = 0;

    bool more() {
        if (i < s.size()) return true;
        if (!pull || !pull()) throw Incomplete();
        return true;
    }
    int depth = 0;

    void skipWs() { while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i; }
    bool at(const char* t) const { return s.compare(i, strlen(t), t) == 0; }
    bool keyword(const char* k) const {
        size_t n = strlen(k);
        if (i + n > s.size() || !equalNoCase(s.c_str() + i, k, n)) return false;
        return i + n == s.size() || isDelim(s[i + n]) || s[i + n] == '(' || s[i + n] == '\n' || (k[0] == 'r' && s[i + n] == '.');
    }
    static NodeP bin(Node::Kind k, NodeP a, NodeP b) {
        auto n = std::make_unique<Node>();
        n->kind = k;
        n->a = std::move(a);
        n->b = std::move(b);
        return n;
    }

    NodeP seq() {
        NodeP n = andOr();
        for (;;) {
            skipWs();
            if (i < s.size() && s[i] == '&' && !at("&&")) { ++i; n = bin(Node::Seq, std::move(n), andOr()); }
            else return n;
        }
    }
    NodeP andOr() {
        NodeP n = pipe();
        for (;;) {
            skipWs();
            if (at("&&")) { i += 2; n = bin(Node::And, std::move(n), pipe()); }
            else if (at("||")) { i += 2; n = bin(Node::Or, std::move(n), pipe()); }
            else return n;
        }
    }
    NodeP pipe() {
        NodeP n = primary();
        for (;;) {
            skipWs();
            if (i < s.size() && s[i] == '|' && !at("||")) { ++i; n = bin(Node::Pipe, std::move(n), primary()); }
            else return n;
        }
    }

    NodeP primary() {
        auto n = std::make_unique<Node>();
        for (;;) {
            while (i < s.size() && (isDelim(s[i]) || (s[i] == '\n' && depth > 0))) ++i;
            if (i < s.size() && s[i] == '@') { ++i; n->quiet = true; continue; }
            break;
        }
        if (i < s.size() && s[i] == '(') {
            ++i;
            ++depth;
            n->kind = Node::Block;
            for (;;) {
                while (i < s.size() && (isDelim(s[i]) || s[i] == '\n')) ++i;
                if (i >= s.size()) { more(); continue; }
                if (s[i] == ')') { ++i; break; }
                n->list.push_back(seq());
                skipWs();
                if (i < s.size() && s[i] == '\n') continue;
                if (i < s.size() && s[i] == ')') { ++i; break; }
                if (i >= s.size()) more();
            }
            --depth;
            redirsOnly(*n);
            return n;
        }
        if (i < s.size() && (s[i] == ':' || (s[i] == ')' && depth == 0))) { skipToLineEnd(); return n; }
        auto help = [&](size_t len) { size_t k = i + len; while (k < s.size() && isDelim(s[k])) ++k; return s.compare(k, 2, "/?") == 0; };
        if ((keyword("rem") && help(3)) || (keyword("if") && help(2)) || (keyword("for") && help(3))) { simple(*n); return n; }
        if (keyword("rem")) { size_t from = i; skipToLineEnd(); n->text = s.substr(from, i - from); return n; }
        if (keyword("if")) return parseIf(std::move(n));
        if (keyword("for")) return parseFor(std::move(n));
        simple(*n);
        return n;
    }

    void skipToLineEnd() { while (i < s.size() && s[i] != '\n') ++i; }

    std::string word(bool stopAtEq = false) {
        skipWs();
        std::string w;
        bool q = false;
        while (i < s.size()) {
            char c = s[i];
            if (!q && (c == ' ' || c == '\t' || c == '\n' || c == '&' || c == '|' || c == '<' || c == '>' || (c == ')' && depth > 0))) break;
            if (!q && stopAtEq && c == '=' && i + 1 < s.size() && s[i + 1] == '=') break;
            if (!q && c == '^' && i + 1 < s.size()) { w += s[i + 1]; i += 2; continue; }
            if (c == '"') q = !q;
            w += c;
            ++i;
        }
        return w;
    }

    bool redir(Node& n) {
        size_t save = i;
        int fd = 1;
        if (i < s.size() && isdigit((unsigned char)s[i]) && i + 1 < s.size() && (s[i + 1] == '>' || s[i + 1] == '<')) { fd = s[i] - '0'; ++i; }
        if (i >= s.size() || (s[i] != '>' && s[i] != '<')) { i = save; return false; }
        Redir r;
        if (s[i] == '<') { r.fd = 0; ++i; }
        else {
            r.fd = fd;
            ++i;
            if (i < s.size() && s[i] == '>') { r.append = true; ++i; }
        }
        skipWs();
        if (i + 1 < s.size() && s[i] == '&' && isdigit((unsigned char)s[i + 1])) { r.dupTo = s[i + 1] - '0'; i += 2; }
        else r.target = word();
        n.redirs.push_back(r);
        return true;
    }
    void redirsOnly(Node& n) { for (;;) { skipWs(); if (!redir(n)) break; } }

    void simple(Node& n) {
        bool q = false;
        while (i < s.size()) {
            char c = s[i];
            if (c == '\n') break;
            if (!q) {
                if (c == '&' || c == '|' || (c == ')' && depth > 0)) break;
                if (c == '^') { if (i + 1 < s.size() && s[i + 1] != '\n') n.text += s[i + 1]; i += 2; continue; }
                if ((c == '>' || c == '<' || (isdigit((unsigned char)c) && i + 1 < s.size() && (s[i + 1] == '>' || s[i + 1] == '<') &&
                     (i == 0 || s[i - 1] == ' ' || s[i - 1] == '\t'))) && redir(n)) continue;
            }
            if (c == '"') q = !q;
            n.text += c;
            ++i;
        }
    }

    NodeP parseIf(NodeP n) {
        n->kind = Node::If;
        i += 2;
        for (;;) {
            skipWs();
            if (keyword("/i")) { n->ifI = true; i += 2; continue; }
            if (keyword("not")) { n->ifNot = true; i += 3; continue; }
            break;
        }
        const char* kinds[] = { "errorlevel", "exist", "defined", "cmdextversion" };
        for (const char* k : kinds) if (keyword(k)) { n->ifKind = upper(k); i += strlen(k); n->ifA = word(); break; }
        if (n->ifKind.empty()) {
            n->ifKind = "CMP";
            n->ifA = word(true);
            skipWs();
            if (at("==")) { n->ifOp = "=="; while (i < s.size() && s[i] == '=') ++i; }
            else n->ifOp = upper(word());
            skipWs();
            n->ifB = word();
            if (n->ifA.empty() || n->ifB.empty()) syntaxError();
        }
        skipWs();
        if (i < s.size() && s[i] == '(') {
            n->body = primary();
            size_t save = i;
            while (i < s.size() && isDelim(s[i])) ++i;
            if (keyword("else")) { i += 4; skipWs(); n->els = seq(); }
            else i = save;
        }
        else n->body = seq();
        return n;
    }

    NodeP parseFor(NodeP n) {
        n->kind = Node::For;
        i += 3;
        for (;;) {
            skipWs();
            if (i + 1 < s.size() && s[i] == '/') {
                char c = (char)toupper((unsigned char)s[i + 1]);
                n->forSwitch += c;
                i += 2;
                if (c == 'R') { skipWs(); if (i < s.size() && s[i] != '%') n->forRoot = unquote(word()); }
                if (c == 'F') { skipWs(); if (i < s.size() && s[i] == '"') n->forOpts = unquote(word()); }
                continue;
            }
            break;
        }
        skipWs();
        if (i + 1 >= s.size() || s[i] != '%') syntaxError();
        n->forVar = s.substr(i, 2);
        i += 2;
        skipWs();
        if (!keyword("in")) syntaxError();
        i += 2;
        skipWs();
        if (i >= s.size() || s[i] != '(') syntaxError();
        ++i;
        bool q = false;
        for (;;) {
            while (i < s.size() && (q || s[i] != ')')) {
                if (s[i] == '"') q = !q;
                if (!q && s[i] == '^' && i + 1 < s.size()) ++i;
                n->forSet += s[i] == '\n' ? ' ' : s[i];
                ++i;
            }
            if (i < s.size()) break;
            more();
        }
        ++i;
        skipWs();
        if (!keyword("do")) syntaxError();
        i += 2;
        n->body = seq();
        return n;
    }
};

}
