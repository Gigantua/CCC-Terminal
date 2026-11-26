#pragma once
#include "shell.h"

namespace ccc {

inline bool Shell::continues(const std::string& line) {
    return (line.size() - (line.find_last_not_of('^') + 1)) % 2 == 1;
}

inline bool Shell::confirmTerminateBatch() {
    if (terminatingBatch) return true;
    for (;;) {
        interrupted = false;
        stdoutSink().write(message(Msg::TerminateBatch));
        OutputBuffer::flushAll();
        std::string answer;
        bool got = false;
        try { got = host.readLine(answer); } catch (Interrupt&) { got = false; }
        answer = trim(answer);
        char key = answer.size() == 1 ? answerKey(answer, Msg::NoYesKeys, "NY") : 0;
        if (!got || key == 'Y') { terminatingBatch = true; return true; }
        if (key == 'N') return false;
    }
}

inline bool Shell::nextBatchLine(Batch& b, std::string& out) {
    out.clear();
    for (;;) {
        if (b.pc >= b.lines->size()) return !out.empty();
        std::string l = (*b.lines)[b.pc++];
        if (continues(l)) { l.pop_back(); out += l; continue; }
        out += l;
        return true;
    }
}

inline bool Shell::readLogical(std::string& out, Batch* b) {
    if (b) return nextBatchLine(*b, out);
    out.clear();
    std::string l;
    for (;;) {
        OutputBuffer::flushAll();
        if (!host.readLine(l)) return !out.empty();
        if (continues(l)) { l.pop_back(); out += l; stdoutSink().write(message(Msg::More)); continue; }
        out += l;
        return true;
    }
}

inline size_t Shell::findLabel(Batch& b, const std::string& label) {
    if (!b.labels) {
        b.labels = std::make_shared<std::unordered_map<std::string, std::vector<size_t>>>();
        for (size_t k = 0; k < b.lines->size(); ++k) {
            std::string l = trim((*b.lines)[k]);
            if (l.size() < 2 || l[0] != ':') continue;
            auto words = splitArgs(l.substr(1));
            if (!words.empty()) (*b.labels)[upper(words[0])].push_back(k);
        }
    }
    auto it = b.labels->find(upper(label));
    if (it == b.labels->end()) return std::string::npos;
    for (size_t k : it->second) if (k >= b.pc) return k;
    return it->second.front();
}

inline int Shell::runBatch(Batch& b) {
    bool topLevel = batches.empty();
    bool echoBefore = echoOn;
    struct RestoreEcho { Shell* s; bool top, echo; ~RestoreEcho() { if (top && !s->batch()) s->echoOn = echo; } } restoreEcho{ this, topLevel, echoBefore };
    batches.push_back(&b);
    int rc = 0;
    try {
        std::string line;
        while (nextBatchLine(b, line)) {
            checkInterrupt();
            std::string t = trim(line);
            if (t.empty() || t[0] == ':') continue;
            try { runLine(line, &b, echoOn); rc = errorlevel; }
            catch (Interrupt&) {
                if (confirmTerminateBatch()) throw;
                continue;
            }
            if (unwind == Unwind::Goto) { unwind = Unwind::None; continue; }
            if (unwind == Unwind::ExitBatch) { rc = unwindCode; unwind = Unwind::None; break; }
        }
    }
    catch (...) { while (b.localDepth-- > 0) endlocal(); batches.pop_back(); throw; }
    while (b.localDepth-- > 0) endlocal();
    batches.pop_back();
    errorlevel = rc;
    return rc;
}

inline std::shared_ptr<std::vector<std::string>> Shell::readScript(const std::string& path) {
    std::string text;
    if (!readFile(path, text)) return nullptr;
    auto lines = std::make_shared<std::vector<std::string>>();
    size_t pos = 0;
    while (pos < text.size()) {
        size_t e = text.find('\n', pos);
        if (e == std::string::npos) e = text.size();
        lines->emplace_back(text, pos, e - pos);
        std::string& line = lines->back();
        line.erase(std::remove(line.begin(), line.end(), '\r'), line.end());
        pos = e + 1;
    }
    return lines;
}

inline int Shell::runBatchFile(const std::string& path, const std::vector<std::string>& args, const std::string& tail, Ctx* ctx) {
    os::FileInfo fd;
    if (!os::findFile(path, fd) || (fd.attr & os::ATTR_DIRECTORY)) { stderrSink().line(message(Msg::BatchNotFound)); return 1; }
    size_t slash = path.find_last_of(os::SLASHES);
    std::string diskPath = path.substr(0, slash == std::string::npos ? 0 : slash + 1) + fd.name;
    uint64_t size = fd.size;
    uint64_t stamp = (uint64_t)fd.written.time_since_epoch().count();
    Script& s = scripts[os::foldName(diskPath)];
    if (!s.lines || s.size != size || s.stamp != stamp) {
        auto lines = readScript(path);
        if (!lines) { stderrSink().line(message(Msg::BatchNotFound)); return 1; }
        s = Script{ size, stamp, lines, nullptr };
    }
    Batch b;
    b.path = diskPath;
    b.lines = s.lines;
    b.labels = s.labels;
    b.args = args;
    b.tail = tail;
    b.ctx = ctx;
    b.isCmd = path.size() > 4 && ieq(std::string_view(path).substr(path.size() - 4), ".cmd");
    int rc = runBatch(b);
    if (!s.labels && s.lines == b.lines) s.labels = b.labels;
    return rc;
}

}
