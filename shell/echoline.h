#pragma once
#include "parser.h"

namespace ccc {

inline std::string redirectionText(const std::vector<Redir>& redirs) {
    std::string out;
    for (const Redir& r : redirs) {
        out += ' ';
        out += (char)('0' + r.fd);
        out += r.fd == 0 ? "<" : r.append ? ">>" : ">";
        out += r.dupTo >= 0 ? "&" + std::to_string(r.dupTo) : r.target;
    }
    return out;
}

inline std::string forSetText(const Node& n) {
    if (n.forSwitch.find('F') != std::string::npos) return n.forSet;
    std::string out;
    for (size_t a = n.forSet.find_first_not_of(" \t,;="); a != std::string::npos; a = n.forSet.find_first_not_of(" \t,;=", a)) {
        size_t b = n.forSet.find_first_of(" \t,;=", a);
        if (b == std::string::npos) b = n.forSet.size();
        if (!out.empty()) out += ' ';
        out += n.forSet.substr(a, b - a);
        a = b;
    }
    return out;
}

inline bool quietLine(const Node& n) { return n.a ? quietLine(*n.a) : n.quiet; }

inline std::string echoText(const Node& n) {
    switch (n.kind) {
    case Node::Cmd: {
        size_t start = n.text.find_first_not_of(" \t");
        std::string text = start == std::string::npos ? std::string() : n.text.substr(start);
        bool spaced = text.find_first_of(" \t,;=(") != std::string::npos || !n.redirs.empty();
        return text + redirectionText(n.redirs) + (spaced ? " " : "");
    }
    case Node::Seq: return echoText(*n.a) + " & " + echoText(*n.b);
    case Node::And: return echoText(*n.a) + " && " + echoText(*n.b);
    case Node::Or: return echoText(*n.a) + " || " + echoText(*n.b);
    case Node::Pipe: return echoText(*n.a) + " | " + echoText(*n.b);
    case Node::Block: {
        std::string out = "(";
        if (n.list.size() == 1) out += echoText(*n.list[0]);
        else {
            out += "\r\n";
            for (size_t k = 0; k < n.list.size(); ++k) out += (k ? " \r\n " : "") + echoText(*n.list[k]);
            out += "\r\n";
        }
        return out + ")" + redirectionText(n.redirs) + " ";
    }
    case Node::If: {
        std::string out = "if ";
        if (n.ifI) out += "/I ";
        if (n.ifNot) out += "not ";
        out += n.ifKind == "CMP" ? n.ifA + " " + n.ifOp + " " + n.ifB : lower(n.ifKind) + " " + n.ifA;
        out += " " + echoText(*n.body);
        if (n.els) out += " else " + echoText(*n.els);
        return out;
    }
    case Node::For: {
        std::string out = "for ";
        for (char c : n.forSwitch) {
            out += c == 'D' ? std::string("/ ") : std::string("/") + c + " ";
            if (c == 'R' && !n.forRoot.empty()) out += n.forRoot + " ";
            if (c == 'F' && !n.forOpts.empty()) out += "\"" + n.forOpts + "\" ";
        }
        return out + n.forVar + " in (" + forSetText(n) + ") do " + echoText(*n.body);
    }
    }
    return std::string();
}

}
