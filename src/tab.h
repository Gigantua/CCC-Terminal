#pragma once
#include <unordered_map>
#include <unordered_set>
#include "channel.h"
#include "terminal.h"

inline void putInput(Packet& p, const InputEvent& e) {
    p.put((int32_t)e.kind).put((int32_t)e.value).put((int32_t)e.x).put((int32_t)e.y).put(e.ctrl).put(e.shift).put(e.alt).put(e.release).put(e.repeat);
    p.put((int32_t)e.scan).put((uint32_t)e.state).put(e.ch).put(e.text).put(e.base).put(e.shifted).put(e.super);
}

inline InputEvent getInput(Unpacker& u) {
    InputEvent e{ (InputEvent::Kind)u.get<int32_t>() };
    e.value = u.get<int32_t>();
    e.x = u.get<int32_t>();
    e.y = u.get<int32_t>();
    e.ctrl = u.get<bool>();
    e.shift = u.get<bool>();
    e.alt = u.get<bool>();
    e.release = u.get<bool>();
    e.repeat = u.get<bool>();
    e.scan = u.get<int32_t>();
    e.state = u.get<uint32_t>();
    e.ch = u.get<uint32_t>();
    e.text = u.text<std::string>();
    e.base = u.get<uint32_t>();
    e.shifted = u.get<uint32_t>();
    e.super = u.get<bool>();
    return e;
}

struct PictureWire {
    uint64_t key;
    int32_t col, row, offX, offY, cols, rows, srcX, srcY, srcW, srcH, z, destW, destH;
};

using BitmapCache = std::unordered_map<uint64_t, std::shared_ptr<const Bitmap>>;

inline void putView(Packet& p, const Console::Frame& f, const screen::Highlight& h, uint32_t fg, uint32_t bg) {
    p.put((int32_t)f.cols).put((int32_t)f.rows).put(f.cells).put(f.attrs).put((uint32_t)f.clusters.size());
    for (const std::u32string& c : f.clusters) p.put(c);
    p.put((int32_t)f.cursorRow).put((int32_t)f.cursorCol).put(f.thumbPos).put(f.thumbSize).put(f.blinking).put(h).put(fg).put(bg);
    p.put(f.palette).put(f.fg).put(f.bg).put(f.cursorColor).put(f.pointer);
    std::vector<PictureWire> wire;
    for (const Picture& q : f.pictures)
        if (q.bitmap) wire.push_back({ q.bitmap->key, q.col, q.row, q.offX, q.offY, q.cols, q.rows, q.srcX, q.srcY, q.srcW, q.srcH, q.z, q.destW, q.destH });
    p.put(wire).put(f.extraCursors).put(f.extraTextColor).put(f.extraCursorColor);
}

inline void getView(Unpacker& u, screen::View& v, const BitmapCache& bitmaps) {
    Console::Frame& f = v.frame;
    f.cols = u.get<int32_t>();
    f.rows = u.get<int32_t>();
    u.vec(f.cells);
    u.vec(f.attrs);
    f.clusters.resize(u.get<uint32_t>());
    for (std::u32string& c : f.clusters) c = u.text<std::u32string>();
    f.cursorRow = u.get<int32_t>();
    f.cursorCol = u.get<int32_t>();
    f.thumbPos = u.get<double>();
    f.thumbSize = u.get<double>();
    f.blinking = u.get<bool>();
    v.highlight = u.get<screen::Highlight>();
    v.fg = u.get<uint32_t>();
    v.bg = u.get<uint32_t>();
    u.vec(f.palette);
    f.fg = u.get<uint32_t>();
    f.bg = u.get<uint32_t>();
    f.cursorColor = u.get<uint32_t>();
    f.pointer = u.text<std::string>();
    std::vector<PictureWire> wire;
    u.vec(wire);
    f.pictures.clear();
    for (const PictureWire& w : wire) {
        auto it = bitmaps.find(w.key);
        if (it == bitmaps.end()) continue;
        Picture q;
        q.bitmap = it->second;
        q.col = w.col; q.row = w.row; q.offX = w.offX; q.offY = w.offY; q.cols = w.cols; q.rows = w.rows;
        q.srcX = w.srcX; q.srcY = w.srcY; q.srcW = w.srcW; q.srcH = w.srcH; q.z = w.z; q.destW = w.destW; q.destH = w.destH;
        f.pictures.push_back(q);
    }
    u.vec(f.extraCursors);
    f.extraTextColor = u.get<uint32_t>();
    f.extraCursorColor = u.get<uint32_t>();
}

inline void putBitmap(Packet& p, const Bitmap& b) { p.put(b.key).put((int32_t)b.width).put((int32_t)b.height).put(b.pixels); }

inline std::shared_ptr<const Bitmap> getBitmap(Unpacker& u) {
    auto b = std::make_shared<Bitmap>();
    b->key = u.get<uint64_t>();
    b->width = u.get<int32_t>();
    b->height = u.get<int32_t>();
    u.vec(b->pixels);
    return b;
}

struct TabOptions {
    os::Handle in = os::stdHandle(0), out = os::stdHandle(1);
    bool lineWrap = true;
    bool exitAfterRun = false;
    std::string run;
};

class TabProcess {
public:
    int run(const TabOptions& o) {
        opt = o;
        channel.in = o.in;
        channel.out = o.out;
        term.setClipboard = [this](const std::string& t) { Packet p(Msg::ClipboardSet); p.put(t); channel.send(p); };
        term.requestClipboard = [this] { Packet p(Msg::ClipboardGet); channel.send(p); };
        term.openUri = [this](const std::string& uri) { Packet p(Msg::Open); p.put(uri); channel.send(p); };
        term.host.con.lineWrap = o.lineWrap;
        term.host.con.onChange = requestFrame;
        term.host.onTitle = [this](const std::string& t) {
            std::lock_guard<std::mutex> l(m);
            title = t;
            Packet p(Msg::Title);
            p.put(t);
            channel.send(p);
        };
        term.host.srv.bell = [this] { Packet p(Msg::Bell); channel.send(p); };
        term.host.srv.notification = [this](const std::string& title, const std::string& body) { Packet p(Msg::Notify); p.put(title).put(body); channel.send(p); };
        term.host.srv.progress = [this](int state, int value) {
            std::lock_guard<std::mutex> l(m);
            progressState = state;
            progressValue = value;
            Packet p(Msg::Progress);
            p.put((int32_t)state).put((int32_t)value);
            channel.send(p);
        };
        term.onExit = [this] { quit = true; wakeFrame(); };
        term.onCloseKey = [this] { parkRequested = true; wakeFrame(); };
        std::thread(&TabProcess::readLoop, this).detach();
        term.start();
        loop();
        term.stop();
        Packet p(Msg::Exit);
        p.put((int32_t)term.exitCode);
        channel.send(p);
        return term.exitCode;
    }

private:
    TabOptions opt;
    Terminal term;
    Channel channel;
    std::atomic<bool> quit{ false }, credit{ true }, parkRequested{ false };
    std::mutex m;
    std::vector<InputEvent> inputs;
    int width = 0, height = 0;
    bool sized = false, measured = false;
    FontMetrics metrics;
    int barWidth = 12;
    std::string family;
    float points = 0;
    bool metricsChanged = false, fontSeen = false;
    std::string cwd;
    std::string title;
    int progressState = 0, progressValue = 0;
    bool attached = false;
    std::unordered_set<uint64_t> sentBitmaps;
    enum Script { WAITING_FOR_SHELL, RUNNING, DONE } script = WAITING_FOR_SHELL;
    double scriptStart = 0;
    uint64_t frames = 0, scriptFrames = 0, scriptLines = 0;

    void readLoop() {
        Msg kind;
        std::vector<char> body;
        while (channel.receive(kind, body)) {
            Unpacker u(body);
            std::lock_guard<std::mutex> l(m);
            switch (kind) {
            case Msg::Input: inputs.push_back(getInput(u)); requestFrame(); break;
            case Msg::Size: width = u.get<int32_t>(); height = u.get<int32_t>(); sized = true; requestFrame(); break;
            case Msg::Metrics:
                metrics = u.get<FontMetrics>();
                barWidth = u.get<int32_t>();
                family = u.text<std::string>();
                points = u.get<float>();
                metricsChanged = measured = true;
                requestFrame();
                break;
            case Msg::Ready: credit = true; wakeFrame(); break;
            case Msg::Close: quit = true; wakeFrame(); break;
            case Msg::Detach: park(); break;
            case Msg::Attach: channel.resume(); credit = true; attached = true; sentBitmaps.clear(); requestFrame(); break;
            default: break;
            }
        }
        quit = true;
        wakeFrame();
    }

    void park() {
        credit = false;
        Packet p(Msg::Detached);
        channel.sendAndHold(p);
    }

    void loop() {
        while (!quit) {
            int animation = term.frame.nextFrameMs;
            waitForChange(false, animation >= 0 ? (unsigned)animation : NO_LIMIT);
            if (animation >= 0) contentChanged = true;
            if (quit) break;
            if (parkRequested.exchange(false)) park();
            std::vector<InputEvent> events;
            int w, h;
            bool ready;
            {
                std::lock_guard<std::mutex> l(m);
                events.swap(inputs);
                if (metricsChanged) {
                    term.metrics = metrics;
                    term.barWidth = barWidth;
                    term.host.srv.cellW = metrics.cellW;
                    term.host.srv.cellH = metrics.cellH;
                    term.host.fontApplied(family, points);
                    if (!fontSeen) { term.defaultPoints = points; fontSeen = true; }
                    metricsChanged = false;
                }
                w = width;
                h = height;
                ready = sized && measured;
                if (attached) {
                    attached = false;
                    cwd.clear();
                    if (!title.empty()) { Packet t(Msg::Title); t.put(title); channel.send(t); }
                    Packet p(Msg::Progress);
                    p.put((int32_t)progressState).put((int32_t)progressValue);
                    channel.send(p);
                }
            }
            if (!ready) continue;
            if (!events.empty()) {
                term.layout(w, h);
                for (const InputEvent& e : events) term.handle(e);
                contentChanged = true;
            }
            sendRequests();
            if (credit && contentChanged.exchange(false)) {
                term.layout(w, h);
                if (!sendBitmaps(term.frame)) break;
                Packet p(Msg::Frame);
                putView(p, term.frame, term.highlight(), term.host.fg, term.host.bg);
                credit = false;
                if (!channel.send(p)) break;
                ++frames;
            }
            stepScript();
        }
    }

    bool sendBitmaps(const Console::Frame& f) {
        std::unordered_set<uint64_t> shown;
        for (const Picture& q : f.pictures) {
            if (!q.bitmap) continue;
            shown.insert(q.bitmap->key);
            if (!sentBitmaps.insert(q.bitmap->key).second) continue;
            Packet p(Msg::Image);
            putBitmap(p, *q.bitmap);
            if (!channel.send(p)) return false;
        }
        for (auto it = sentBitmaps.begin(); it != sentBitmaps.end();) {
            if (shown.count(*it)) { ++it; continue; }
            Packet p(Msg::ImageDrop);
            p.put(*it);
            channel.send(p);
            it = sentBitmaps.erase(it);
        }
        return true;
    }

    void sendRequests() {
        std::string f;
        float pt;
        if (term.host.takeFontRequest(f, pt)) { Packet p(Msg::Font); p.put(f).put(pt); channel.send(p); }
        int c, r;
        if (term.host.takeResize(c, r)) { Packet p(Msg::Resize); p.put((int32_t)c).put((int32_t)r); channel.send(p); }
        std::string dir = term.directory();
        if (!dir.empty() && cwd != dir) { cwd = dir; Packet p(Msg::Cwd); p.put(cwd); channel.send(p); }
    }

    void stepScript() {
        if (opt.run.empty() || script == DONE) return;
        if (script == WAITING_FOR_SHELL && term.idleAtPrompt()) {
            scriptStart = secondsNow();
            scriptFrames = frames;
            scriptLines = term.host.linesTaken + 1;
            term.submitLine(opt.run);
            script = RUNNING;
            return;
        }
        if (script == RUNNING && term.host.linesTaken >= scriptLines && term.idleAtPrompt()) {
            char line[128];
            snprintf(line, sizeof(line), "%.2f ms, %llu frames\r\n", (secondsNow() - scriptStart) * 1000.0, (unsigned long long)(frames - scriptFrames));
            Packet p(Msg::Print);
            p.put(std::string(line));
            channel.send(p);
            script = DONE;
            if (opt.exitAfterRun) quit = true;
        }
    }
};
