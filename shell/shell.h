#pragma once
#include "environment.h"
#include "io.h"
#include "messages.h"
#include "echoline.h"
#include "dirindex.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <array>
#include <string_view>
#include <unordered_map>
#include <mutex>
#include <thread>

namespace ccc {

class Shell {
public:
    struct Batch;
    struct Ctx { Sink out, err; Source in; };
    explicit Shell(Host& h) : host(h) {
        createStdBuffers();
        srand((unsigned)std::chrono::steady_clock::now().time_since_epoch().count());
        startupEnv = os::environment();
    }

    int errorlevel = 0;
    bool echoOn = true;
    bool delayed = false;
    bool extensions = true;
    bool verify = false;
    std::string cmdline = "ccc.exe";

    Sink stdoutSink() { Sink s; if (host.console()) s.con = host.console(); else s.h = os::stdHandle(1); s.out = outBuffer.get(); return s; }
    Sink stderrSink() { Sink s; if (host.console()) s.con = host.console(); else s.h = os::stdHandle(2); s.out = errBuffer ? errBuffer.get() : outBuffer.get(); return s; }
    Source stdinSource() { return Source{ host.console() ? os::NO_HANDLE : os::stdHandle(0) }; }

    int run() {
        for (;;) {
            if (echoOn) stdoutSink().write("\r\n" + prompt());
            std::string line;
            try {
                atPrompt = true;
                bool got = readLogical(line, nullptr);
                atPrompt = false;
                if (!got) return errorlevel;
                interrupted = false;
                terminatingBatch = false;
                for (const std::string& part : expandMacro(line)) {
                    showRunningTitle(part);
                    runLine(part, nullptr);
                    unwind = Unwind::None;
                }
                restoreTitle();
            }
            catch (ExitShell& e) { return e.code; }
            catch (Interrupt&) { interrupted = false; atPrompt = false; restoreTitle(); }
        }
    }

    std::atomic<bool> interrupted{ false };
    std::atomic<bool> atPrompt{ false };
    void markInterrupted() { interrupted = true; }
    void interrupt() {
        interrupted = true;
        {
            std::lock_guard<std::mutex> lock(childMutex);
            for (const os::Process& p : children) os::interruptProcess(p);
        }
        host.cancelInput();
    }

    int runLine(const std::string& raw, Batch* b, bool echo = false) {
        std::string expanded = expandPercent(raw, b);
        auto pull = [&] {
            std::string more;
            if (b) { if (!nextBatchLine(*b, more)) return false; }
            else { stdoutSink().write(message(Msg::More)); OutputBuffer::flushAll(); if (!host.readLine(more)) return false; }
            expanded += '\n';
            expanded += expandPercent(more, b);
            return true;
        };
        try {
            Parser p(expanded, pull);
            NodeP n = p.parse();
            Ctx c = b && b->ctx ? *b->ctx : Ctx{ stdoutSink(), stderrSink(), stdinSource() };
            if (echo && !quietLine(*n)) c.out.write("\r\n" + prompt() + echoText(*n) + "\r\n");
            return exec(n.get(), c);
        }
        catch (Incomplete&) { return errorlevel; }
        catch (SyntaxError&) { stderrSink().line(message(Msg::SyntaxIncorrect)); errorlevel = 1; return 1; }
    }

    int runBatchFile(const std::string& path, const std::vector<std::string>& args, const std::string& tail, Ctx* ctx);

private:
    Host& host;
    std::unique_ptr<OutputBuffer> outBuffer, errBuffer;
    Environment env;
    DirIndex exeDirs;
    struct Script { uint64_t size = 0, stamp = 0; std::shared_ptr<std::vector<std::string>> lines; std::shared_ptr<std::unordered_map<std::string, std::vector<size_t>>> labels; };
    std::unordered_map<std::string, Script> scripts;
    std::shared_ptr<std::vector<std::string>> readScript(const std::string& path);
    void prepareChild() {
        OutputBuffer::flushAll();
        env.sync();
    }
    std::mutex childMutex;

    void createStdBuffers() {
        if (Console* con = host.console()) { outBuffer = std::make_unique<OutputBuffer>(con, os::NO_HANDLE); return; }
        os::Handle out = os::stdHandle(1), err = os::stdHandle(2);
        outBuffer = std::make_unique<OutputBuffer>(nullptr, out);
        bool shared = out == err || (os::isConsole(out) && os::isConsole(err));
        if (!shared) errBuffer = std::make_unique<OutputBuffer>(nullptr, err);
    }
    os::EnvList startupEnv;
    std::vector<os::Process> children;
    void checkInterrupt() { if (interrupted.load(std::memory_order_relaxed)) throw Interrupt(); }

public:
    struct Batch {
        std::string path;
        std::shared_ptr<std::vector<std::string>> lines;
        std::vector<std::string> args;
        std::string tail;
        Ctx* ctx = nullptr;
        size_t pc = 0;
        int localDepth = 0;
        bool isCmd = false;
        std::shared_ptr<std::unordered_map<std::string, std::vector<size_t>>> labels;
    };
private:
    std::vector<Batch*> batches;
    struct Local { std::vector<std::pair<std::string, std::string>> env; bool delayed, extensions; std::string cwd; };
    std::vector<Local> locals;
    std::vector<std::string> dirStack;
    struct ForVars {
        std::string values[128];
        uint64_t bits[2] = {};
        static unsigned slot(char c) { return (unsigned char)c & 127; }
        bool empty() const { return !(bits[0] | bits[1]); }
        bool has(char c) const { unsigned u = (unsigned char)c; return u < 128 && (bits[u >> 6] >> (u & 63)) & 1; }
        const std::string& get(char c) const { return values[slot(c)]; }
        void set(char c, std::string_view v) { unsigned u = slot(c); values[u].assign(v.data(), v.size()); bits[u >> 6] |= 1ull << (u & 63); }
        void unset(char c) { unsigned u = slot(c); values[u].clear(); bits[u >> 6] &= ~(1ull << (u & 63)); }
    };
    ForVars forVars;
    struct ForScope {
        ForVars& vars;
        char first;
        int count;
        uint64_t bits[2];
        std::string saved[32];
        ForScope(ForVars& v, char c, int n) : vars(v), first(c), count(n) {
            bits[0] = v.bits[0];
            bits[1] = v.bits[1];
            for (int k = 0; k < count; ++k) saved[k] = v.get((char)(first + k));
        }
        ~ForScope() {
            for (int k = 0; k < count; ++k) vars.values[ForVars::slot((char)(first + k))] = std::move(saved[k]);
            vars.bits[0] = bits[0];
            vars.bits[1] = bits[1];
        }
    };

    Batch* batch() { return batches.empty() ? nullptr : batches.back(); }
    size_t findLabel(Batch& b, const std::string& label);

    enum class Unwind : uint8_t { None, Goto, ExitBatch };
    Unwind unwind = Unwind::None;
    int unwindCode = 0;
    bool unwinding() const { return unwind != Unwind::None; }
    std::map<std::string, std::pair<std::string, std::string>> macros;
public:
    void clearMacros() { macros.clear(); }
private:
    bool defineMacro(const std::string& definition);
    std::vector<std::string> expandMacro(const std::string& line);

    std::string baseTitle;
    bool titleShown = false;
    void showRunningTitle(const std::string& line) {
        std::string shown = trim(line);
        if (shown.empty()) return;
        if (baseTitle.empty()) baseTitle = host.windowTitle();
        host.setTitle(baseTitle + " - " + shown);
        titleShown = true;
    }
    void restoreTitle() {
        if (!titleShown) return;
        host.setTitle(baseTitle);
        titleShown = false;
    }

public:
    bool unicodeOutput = false;

    void applyRegistryDefaults() {
        uint32_t v;
        for (int scope : { 0, 1 }) {
            if (os::shellSetting(scope, "DelayedExpansion", v)) delayed = v == 1;
            if (os::shellSetting(scope, "EnableExtensions", v)) extensions = v != 0;
        }
    }

    void applyDefaultColor() {
        uint32_t v = 0, found = 0;
        for (int scope : { 0, 1 })
            if (os::shellSetting(scope, "DefaultColor", v) && v) found = v;
        if (found && (found >> 4 & 15) != (found & 15)) host.setColor((int)(found & 0xFF));
    }

    void runAutoRun() {
        for (int scope : { 0, 1 }) {
            std::string command = os::shellSettingText(scope, "AutoRun");
            if (!command.empty()) runLine(command, nullptr);
        }
    }

    void setUnicodeOutput(bool on) {
        unicodeOutput = on;
        if (host.console()) return;
        if (!os::isConsole(os::stdHandle(1))) outBuffer->setUtf16(on);
        if (errBuffer && !os::isConsole(os::stdHandle(2))) errBuffer->setUtf16(on);
    }

private:
    int exitBatch(int code) { unwind = Unwind::ExitBatch; unwindCode = code; lastExitCode = code; return code; }
    int lastExitCode = 0;
    bool terminatingBatch = false;
    bool confirmTerminateBatch();
    static std::string appDir() {
        std::string p = os::selfPath();
        return p.substr(0, p.find_last_of(os::SLASH) + 1);
    }
    static std::string clockText(char sep) {
        os::Clock t = os::localNow();
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%2d:%02d:%02d%c%02d", t.hour, t.minute, t.second, sep, t.millis / 10);
        return buf;
    }

    bool nextBatchLine(Batch& b, std::string& out);
    bool readLogical(std::string& out, Batch* b);
    static bool continues(const std::string& line);

public:
    bool getVar(const std::string& name, std::string& value);
    void setVar(const std::string& name, const std::string* value);
    static std::string cwd() { return os::currentDirectory(); }
    static std::string fullPath(const std::string& p) { return os::fullPath(p); }
    static std::string dateText() { return os::shortDate(os::localNow()); }

private:

    static std::string modify(const std::string& v, const std::string& spec);

    std::string tildeMods(const std::string& mods, const std::string& value);
    static std::vector<std::string> splitList(const std::string& s, char separator = ';') {
        std::vector<std::string> out;
        size_t a = 0;
        while (a <= s.size()) {
            size_t b = s.find(separator, a);
            if (b == std::string::npos) b = s.size();
            if (b > a) out.push_back(unquote(s.substr(a, b - a)));
            a = b + 1;
        }
        return out;
    }
    static std::string fileTime(os::FileTime ft);

    std::string batchArg(Batch* b, int k);

    std::string expandPercent(const std::string& s, Batch* b);

    std::string expandLate(const std::string& s);
    std::string expandForVars(const std::string& s);

    struct Handles {
        std::vector<os::Handle> h;
        std::vector<std::unique_ptr<OutputBuffer>> buffers;
        ~Handles() {
            buffers.clear();
            for (os::Handle x : h) os::close(x);
        }
        OutputBuffer* bufferFor(os::Handle handle, bool utf16) {
            buffers.push_back(std::make_unique<OutputBuffer>(nullptr, handle));
            buffers.back()->setUtf16(utf16);
            return buffers.back().get();
        }
    };

    bool applyRedirs(const std::vector<Redir>& rs, Ctx& c, Handles& hs);

    int exec(Node* n, Ctx& c);

    int execPipe(Node* n, Ctx& c);

    static bool isNumber(const std::string& s, long long& v) {
        if (s.empty()) return false;
        char* e;
        v = strtoll(s.c_str(), &e, 0);
        return *e == 0;
    }
    int execIf(Node* n, Ctx& c);
    static bool hasMatch(const std::string& pattern);

    static std::vector<os::FileInfo> list(const std::string& pattern) { return os::listFiles(pattern); }
    static bool attrMatch(uint32_t a, const std::string& spec) {
        for (size_t k = 0; k < spec.size(); ++k) {
            bool neg = spec[k] == '-';
            if (neg && ++k >= spec.size()) break;
            uint32_t bit = 0;
            switch (spec[k]) {
            case 'D': bit = os::ATTR_DIRECTORY; break;
            case 'H': bit = os::ATTR_HIDDEN; break;
            case 'S': bit = os::ATTR_SYSTEM; break;
            case 'R': bit = os::ATTR_READONLY; break;
            case 'A': bit = os::ATTR_ARCHIVE; break;
            case 'I': bit = os::ATTR_NOT_INDEXED; break;
            case 'L': bit = os::ATTR_REPARSE; break;
            case 'O': bit = os::ATTR_OFFLINE; break;
            default: continue;
            }
            if (((a & bit) != 0) == neg) return false;
        }
        return true;
    }
    static std::string dirOf(const std::string& p) {
        size_t s = p.find_last_of(os::PATH_STOPS);
        return s == std::string::npos ? "" : p.substr(0, s + 1);
    }
    static bool wild(const std::string& s) { return s.find_first_of("*?") != std::string::npos; }

    int runBody(Node* n, Ctx& c, char var, std::string_view value);

    int execFor(Node* n, Ctx& c);

    int forF(Node* n, Ctx& c, char var, const std::string& set);

    using Builtin = int (Shell::*)(const std::string&, Ctx&);
    enum class Reset : uint8_t { Never, Always, OutsideBatch };
    struct BuiltinInfo { Builtin fn; Reset reset; };
    static const BuiltinInfo* findBuiltin(const char* name, size_t len) {
        static const std::unordered_map<std::string_view, BuiltinInfo> m = {
            { "CD", { &Shell::cmdCd, Reset::Always } }, { "CHDIR", { &Shell::cmdCd, Reset::Always } },
            { "MD", { &Shell::cmdMd, Reset::Always } }, { "MKDIR", { &Shell::cmdMd, Reset::Always } },
            { "RD", { &Shell::cmdRd, Reset::Never } }, { "RMDIR", { &Shell::cmdRd, Reset::Never } },
            { "PUSHD", { &Shell::cmdPushd, Reset::Always } }, { "POPD", { &Shell::cmdPopd, Reset::Always } },
            { "DIR", { &Shell::cmdDir, Reset::Always } }, { "COPY", { &Shell::cmdCopy, Reset::Always } },
            { "MOVE", { &Shell::cmdMove, Reset::Always } }, { "DEL", { &Shell::cmdDel, Reset::Always } },
            { "ERASE", { &Shell::cmdDel, Reset::Always } }, { "REN", { &Shell::cmdRen, Reset::Never } },
            { "RENAME", { &Shell::cmdRen, Reset::Never } }, { "TYPE", { &Shell::cmdType, Reset::Always } },
            { "MKLINK", { &Shell::cmdMklink, Reset::Always } }, { "ASSOC", { &Shell::cmdAssoc, Reset::OutsideBatch } },
            { "FTYPE", { &Shell::cmdFtype, Reset::OutsideBatch } }, { "SET", { &Shell::cmdSet, Reset::OutsideBatch } },
            { "SETLOCAL", { &Shell::cmdSetlocal, Reset::Never } }, { "ENDLOCAL", { &Shell::cmdEndlocal, Reset::Never } },
            { "PATH", { &Shell::cmdPath, Reset::OutsideBatch } }, { "DPATH", { &Shell::cmdDpath, Reset::OutsideBatch } },
            { "PROMPT", { &Shell::cmdPrompt, Reset::OutsideBatch } }, { "TITLE", { &Shell::cmdTitle, Reset::Never } },
            { "COLOR", { &Shell::cmdColor, Reset::Always } }, { "CLS", { &Shell::cmdCls, Reset::Never } },
            { "GOTO", { &Shell::cmdGoto, Reset::Never } }, { "CALL", { &Shell::cmdCall, Reset::Never } },
            { "SHIFT", { &Shell::cmdShift, Reset::Never } }, { "EXIT", { &Shell::cmdExit, Reset::Never } },
            { "ECHO", { &Shell::cmdEcho, Reset::Never } }, { "REM", { &Shell::cmdRem, Reset::Never } },
            { "PAUSE", { &Shell::cmdPause, Reset::Never } }, { "BREAK", { &Shell::cmdRem, Reset::Never } },
            { "START", { &Shell::cmdStart, Reset::Always } }, { "DATE", { &Shell::cmdDate, Reset::Always } },
            { "TIME", { &Shell::cmdTime, Reset::Always } }, { "VER", { &Shell::cmdVer, Reset::Never } },
            { "VOL", { &Shell::cmdVol, Reset::Never } }, { "VERIFY", { &Shell::cmdVerify, Reset::Never } },
            { "FONT", { &Shell::cmdFont, Reset::Never } }, { "LINEWRAP", { &Shell::cmdLinewrap, Reset::Never } },
            { "MORE", { &Shell::cmdMore, Reset::Always } }, { "TIMEOUT", { &Shell::cmdTimeout, Reset::Always } },
            { "CHOICE", { &Shell::cmdChoice, Reset::Never } }, { "CHCP", { &Shell::cmdChcp, Reset::Always } },
            { "MODE", { &Shell::cmdMode, Reset::Always } }, { "DOSKEY", { &Shell::cmdDoskey, Reset::Never } },
        };
        static const auto table = [] {
            std::array<std::pair<uint64_t, const BuiltinInfo*>, 256> t{};
            for (auto& [k, v] : m) {
                uint64_t key = 0;
                std::memcpy(&key, k.data(), k.size());
                size_t h = (size_t)((key * 0x9E3779B97F4A7C15ull) >> 56);
                while (t[h].second) h = (h + 1) & 255;
                t[h] = { key, &v };
            }
            return t;
        }();
        if (len == 0 || len > 8) return nullptr;
        uint64_t key = 0;
        for (size_t k = 0; k < len; ++k) key |= (uint64_t)((unsigned char)name[k] & 0xDF) << (8 * k);
        for (size_t h = (size_t)((key * 0x9E3779B97F4A7C15ull) >> 56);; h = (h + 1) & 255) {
            if (!table[h].second) return nullptr;
            if (table[h].first == key) return table[h].second;
        }
    }

    int command(const std::string& text, Ctx& c, Node* n = nullptr);
    static bool hasHelpSwitch(const std::string& text, size_t from);
    bool resetsErrorlevel(Reset reset);

    int changeDrive(const std::string& d, Ctx& c);

    std::string resolve(const std::string& name0);
    static bool runnable(const std::string& path);

    int external(const std::string& text, Ctx& c);

    int spawn(const std::string& exe, const std::string& cmdline, Ctx& c, bool wait);
    int spawnAttached(ConsoleServer& server, const std::string& exe, const std::string& cmdline, Ctx& c);
    static bool attachable(const Ctx& c);
    int waitChild(os::Process& process);
    int childDone(int code);
    int notRun(Ctx& c, const std::string& text);
    os::EnvList colorEnvironment() const;

public:
    std::string promptText;
    std::string prompt() {
        std::string p = promptText;
        if (p.empty() && !getVar("PROMPT", p)) p = "$P$G";
        std::string o;
        for (size_t i = 0; i < p.size(); ++i) {
            if (p[i] != '$' || i + 1 >= p.size()) { o += p[i]; continue; }
            char c = (char)toupper((unsigned char)p[++i]);
            switch (c) {
            case 'P': o += cwd(); break;
            case 'G': o += '>'; break;
            case 'L': o += '<'; break;
            case 'N': o += os::driveOf(cwd()).substr(0, 1); break;
            case 'T': o += clockText(','); break;
            case 'D': o += dateText(); break;
            case 'V': o += os::versionLine(); break;
            case '_': o += "\r\n"; break;
            case '$': o += '$'; break;
            case 'E': o += '\x1b'; break;
            case 'B': o += '|'; break;
            case 'Q': o += '='; break;
            case 'A': o += '&'; break;
            case 'C': o += '('; break;
            case 'F': o += ')'; break;
            case 'H': o += '\b'; break;
            case 'S': o += ' '; break;
            case '+': o += std::string(dirStack.size(), '+'); break;
            case 'M': break;
            default: break;
            }
        }
        return o;
    }
    static std::string banner() { return os::banner(); }

private:
    int syntaxError(Ctx& c) { c.err.line(message(Msg::SyntaxIncorrect)); return 1; }
    int fail(Ctx& c, int e = os::lastError()) { c.err.line(systemMessage(e)); return 1; }
    unsigned outputPage() {
        if (unicodeOutput) return os::UTF8_PAGE;
        unsigned page = host.console() ? 0 : os::consoleOutputPage();
        return page ? page : unicode::byteCodePage();
    }
    std::string message(Msg id, std::initializer_list<std::string_view> args = {}) { return insertArguments(messageText(id, outputPage()), args); }
    std::string systemMessage(int code) { return os::encodeText(os::errorText(code), outputPage()); }
    char answerKey(const std::string& answer, Msg keys, std::string_view canonical) {
        std::string k = message(keys);
        int a = toupper((unsigned char)(trim(answer) + " ")[0]);
        for (size_t i = 0; i < k.size() && i < canonical.size(); ++i)
            if (toupper((unsigned char)k[i]) == a) return canonical[i];
        return 0;
    }

    static bool isSwitch(const std::string& token, std::string_view known) {
        if (token.empty() || token[0] != '/') return false;
        if constexpr (os::SLASH != '/') return true;
        for (size_t a = 1; a <= token.size();) {
            size_t b = token.find('/', a);
            if (b == std::string::npos) b = token.size();
            std::string_view part(token.data() + a, b - a);
            bool match = false;
            for (size_t s = 0; s < known.size() && !match;) {
                size_t e = known.find('|', s);
                if (e == std::string_view::npos) e = known.size();
                std::string_view spec = known.substr(s, e - s);
                size_t colon = spec.find(':');
                std::string_view name = spec.substr(0, colon);
                if (colon == std::string_view::npos) match = part.size() == name.size() && equalNoCase(part.data(), name.data(), name.size());
                else if (part.size() >= name.size() && equalNoCase(part.data(), name.data(), name.size())) {
                    std::string_view value = part.substr(name.size()), chars = spec.substr(colon + 1);
                    if (!value.empty() && value[0] == ':') value.remove_prefix(1);
                    match = true;
                    for (char ch : value) if (chars.find((char)toupper((unsigned char)ch)) == std::string_view::npos) match = false;
                }
                s = e + 1;
            }
            if (!match) return false;
            a = b + 1;
        }
        return true;
    }

    struct Opts {
        std::string sw;
        std::vector<std::string> sws, pos;
        bool has(char c) const { return sw.find(c) != std::string::npos; }
    };
    static Opts opts(const std::string& args, std::string_view known) {
        Opts o;
        for (auto& a : splitArgs(args)) {
            size_t k = 0;
            if (!isSwitch(a, known)) { o.pos.push_back(a); continue; }
            while (k < a.size() && a[k] == '/') {
                size_t e = a.find('/', k + 1);
                std::string s = upper(a.substr(k + 1, e == std::string::npos ? std::string::npos : e - k - 1));
                o.sws.push_back(s);
                if (s.size() == 1) o.sw += s;
                k = e == std::string::npos ? a.size() : e;
            }
        }
        return o;
    }

    int cmdCd(const std::string& args, Ctx& c);
    int cmdMd(const std::string& args, Ctx& c);
    static bool removeTree(const std::string& d);
    int cmdRd(const std::string& args, Ctx& c);
    int cmdPushd(const std::string& args, Ctx& c);
    int cmdPopd(const std::string&, Ctx&);

    int cmdDir(const std::string& args, Ctx& c);
    char askOverwrite(const std::string& name, Ctx& c);
    int copyFailed(Ctx& c, const std::string& error) { c.err.line(error); c.out.line(message(Msg::FilesCopied, { pad("0", 9) })); return 1; }
    std::array<std::string, 2> volumeLines(const os::Volume& v);
    bool promptOverwrite(const std::vector<std::string>& sws);

    bool readInput(std::string& line, Ctx& c);
    bool readAnswer(std::string& answer, Ctx& c);

    int cmdCopy(const std::string& args, Ctx& c);
    static bool isDevice(const std::string& name, const char* device);
    template <class Emit> void readUntilCtrlZ(Ctx& c, Emit emit);

    int cmdMove(const std::string& args, Ctx& c);

    int cmdDel(const std::string& args, Ctx& c);

    static std::string renameTo(const std::string& src, const std::string& pat);
    int cmdRen(const std::string& args, Ctx& c);

    int cmdType(const std::string& args, Ctx& c);

    int cmdMklink(const std::string& args, Ctx& c);

    int assocLike(const std::string& args, Ctx& c, bool ftype);
    int cmdAssoc(const std::string& a, Ctx& c);
    int cmdFtype(const std::string& a, Ctx& c);

    int cmdSet(const std::string& args0, Ctx& c);

    int setA(const std::string& e, Ctx& c);

    int cmdSetlocal(const std::string& args, Ctx&);
    void pushLocal();
    void endlocal();
    int cmdEndlocal(const std::string&, Ctx&);

    int pathLike(const std::string& var, const std::string& args0, Ctx& c);
    int cmdPath(const std::string& a, Ctx& c);
    int cmdDpath(const std::string& a, Ctx& c);
    int cmdPrompt(const std::string& a, Ctx&);
    int cmdTitle(const std::string& a, Ctx&);
    int cmdColor(const std::string& a, Ctx&);
    int cmdCls(const std::string&, Ctx& c);

    int cmdGoto(const std::string& args, Ctx& c);

    int cmdCall(const std::string& args0, Ctx& c);

    int cmdShift(const std::string& args, Ctx&);
    int cmdExit(const std::string& args, Ctx&);
    int cmdEcho(const std::string& args, Ctx& c);
    int cmdRem(const std::string&, Ctx&);
    int cmdPause(const std::string&, Ctx& c);

    int cmdStart(const std::string& args, Ctx& c);

    int dateTime(const std::string& args, Ctx& c, bool date);
    int cmdDate(const std::string& a, Ctx& c);
    int cmdTime(const std::string& a, Ctx& c);
    int cmdVer(const std::string&, Ctx& c);
    int cmdVol(const std::string& args, Ctx& c);
    int cmdVerify(const std::string& args, Ctx& c);
    int cmdFont(const std::string& args, Ctx& c);
    int cmdMore(const std::string& args, Ctx& c);
    int cmdTimeout(const std::string& args, Ctx& c);
    int cmdChoice(const std::string& args, Ctx& c);
    int cmdChcp(const std::string& args, Ctx& c);
    int cmdMode(const std::string& args, Ctx& c);
    int cmdDoskey(const std::string& args, Ctx& c);
    int cmdLinewrap(const std::string& args, Ctx& c);
    bool consoleToolsBuiltIn() { return host.console() || !os::CONSOLE_TOOLS_EXTERNAL; }

public:
    int runBatch(Batch& b);
};

}
