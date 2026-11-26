#pragma once
#include <memory>
#include "strip.h"
#include "tab.h"

inline std::string quoteArg(const std::string& a) {
    if (!a.empty() && a.find_first_of(" \t\"") == std::string::npos) return a;
    std::string out = "\"";
    size_t slashes = 0;
    for (char c : a) {
        if (c == '\\') { ++slashes; continue; }
        out.append(c == '"' ? slashes * 2 + 1 : slashes, '\\');
        slashes = 0;
        out += c;
    }
    out.append(slashes * 2, '\\');
    return out + "\"";
}

struct TabRequests {
    bool font = false, bell = false, clipboardWanted = false, clipboardSet = false;
    std::string clipboard;
    std::vector<std::string> opens;
    std::vector<std::pair<std::string, std::string>> notifications;
    std::string family;
    float points = 0;
    int cols = 0, rows = 0;
    std::vector<std::string> prints;
};

class Tab {
public:
    screen::View shown;
    std::atomic<bool> active{ false }, activity{ false }, exited{ false };
    std::atomic<int> exitCode{ 0 }, progressState{ 0 }, progressValue{ 0 };

    bool start(const std::string& args, const std::string& dir) {
        os::Handle childIn, childOut;
        if (!os::createPipe(childIn, channel.out, 1 << 16)) return false;
        if (!os::createPipe(channel.in, childOut, 1 << 20)) { os::close(childIn); return false; }
        os::Spawn s;
        s.exe = os::selfPath();
        s.cmdline = quoteArg(s.exe) + " --tab" + args;
        s.dir = dir;
        s.in = childIn;
        s.out = childOut;
        s.hidden = true;
        int error = 0;
        bool ok = os::spawn(s, process, error);
        os::close(childIn);
        os::close(childOut);
        if (!ok) return false;
        reader = std::thread(&Tab::readLoop, this);
        return true;
    }

    void adopt(const TabHandoff& h) {
        channel.in = h.in;
        channel.out = h.out;
        process = h.process;
        resume();
    }

    void resume() {
        if (reader.joinable()) reader.join();
        handedOff = false;
        activity = false;
        reader = std::thread(&Tab::readLoop, this);
        Packet p(Msg::Attach);
        channel.send(p);
    }

    bool parked() const { return handedOff; }

    bool detach(TabHandoff& h) {
        Packet p(Msg::Detach);
        channel.send(p);
        if (reader.joinable()) reader.join();
        if (!handedOff) return false;
        h.in = channel.in;
        h.out = channel.out;
        h.process = process;
        channel.in = channel.out = os::NO_HANDLE;
        process = os::Process();
        return true;
    }

    ~Tab() {
        if (process != os::Process()) {
            close();
            if (!os::waitProcessFor(process, 5000)) os::killProcess(process);
        }
        if (reader.joinable()) reader.join();
        channel.close();
        os::closeProcess(process);
    }

    void close() { Packet p(Msg::Close); channel.send(p); }
    void park() { Packet p(Msg::Detach); channel.send(p); }
    void focus(bool on) { input({ InputEvent::Focus, on ? 1 : 0 }); }
    void input(const InputEvent& e) { Packet p(Msg::Input); putInput(p, e); channel.send(p); }
    void size(int w, int h) { Packet p(Msg::Size); p.put((int32_t)w).put((int32_t)h); channel.send(p); }
    void metrics(const FontMetrics& m, int barWidth, const std::string& family, float points) {
        Packet p(Msg::Metrics);
        p.put(m).put((int32_t)barWidth).put(family).put(points);
        channel.send(p);
    }

    bool takeView() {
        {
            std::lock_guard<std::mutex> l(m);
            if (!fresh) return false;
            std::swap(shown, pending);
            fresh = false;
        }
        Packet p(Msg::Ready);
        channel.send(p);
        return true;
    }

    std::string title() { std::lock_guard<std::mutex> l(m); return titleText; }
    std::string cwd() { std::lock_guard<std::mutex> l(m); return cwdText; }

    TabRequests takeRequests() { std::lock_guard<std::mutex> l(m); return std::exchange(requests, TabRequests()); }

private:
    Channel channel;
    os::Process process;
    std::thread reader;
    std::mutex m;
    screen::View pending;
    bool fresh = false;
    std::atomic<bool> handedOff{ false };
    std::string titleText = "ccc", cwdText;
    TabRequests requests;
    BitmapCache bitmaps;

    void readLoop() {
        Msg kind;
        std::vector<char> body;
        screen::View incoming;
        while (channel.receive(kind, body)) {
            Unpacker u(body);
            if (kind == Msg::Detached) { handedOff = true; return; }
            if (kind == Msg::Frame) {
                getView(u, incoming, bitmaps);
                std::lock_guard<std::mutex> l(m);
                std::swap(pending, incoming);
                fresh = true;
                if (!active) activity = true;
            }
            else {
                std::lock_guard<std::mutex> l(m);
                switch (kind) {
                case Msg::Title: titleText = u.text<std::string>(); break;
                case Msg::Cwd: cwdText = u.text<std::string>(); break;
                case Msg::Font: requests.font = true; requests.family = u.text<std::string>(); requests.points = u.get<float>(); break;
                case Msg::Resize: requests.cols = u.get<int32_t>(); requests.rows = u.get<int32_t>(); break;
                case Msg::Print: requests.prints.push_back(u.text<std::string>()); break;
                case Msg::Exit: exitCode = u.get<int32_t>(); break;
                case Msg::Bell: requests.bell = true; if (!active) activity = true; break;
                case Msg::Notify: { std::string t = u.text<std::string>(); requests.notifications.push_back({ t, u.text<std::string>() }); break; }
                case Msg::Progress: progressState = u.get<int32_t>(); progressValue = u.get<int32_t>(); break;
                case Msg::Image: { auto b = getBitmap(u); bitmaps[b->key] = b; break; }
                case Msg::ImageDrop: bitmaps.erase(u.get<uint64_t>()); break;
                case Msg::ClipboardSet: requests.clipboardSet = true; requests.clipboard = u.text<std::string>(); break;
                case Msg::ClipboardGet: requests.clipboardWanted = true; break;
                case Msg::Open: requests.opens.push_back(u.text<std::string>()); break;
                default: break;
                }
            }
            requestFrame();
        }
        exited = true;
        requestFrame();
    }
};

struct ClosedTab {
    std::unique_ptr<Tab> live;
    std::string cwd;
    int index = 0;
    double closedAt = 0;
};

inline void endInBackground(std::unique_ptr<Tab> t) {
    if (!t) return;
    t->close();
    std::thread([dying = std::move(t)] {}).detach();
}

class Tabs {
public:
    static constexpr int MAX_CLOSED = 10, MAX_PARKED = 5;
    static constexpr double PARK_SECONDS = 15;
    std::vector<std::unique_ptr<Tab>> list;
    std::vector<ClosedTab> closed;
    int active = 0;

    Tab* current() { return list.empty() ? nullptr : list[active].get(); }

    bool open(const std::string& args) { return open(args, current() ? current()->cwd() : std::string(), (int)list.size()); }

    bool open(const std::string& args, const std::string& dir, int index) {
        auto t = std::make_unique<Tab>();
        if (!t->start(args, dir)) return false;
        insert(std::move(t), index);
        return true;
    }

    bool reopen(const std::string& args) {
        if (closed.empty()) return false;
        ClosedTab c = std::move(closed.back());
        closed.pop_back();
        if (!c.live) return open(args, c.cwd, c.index);
        c.live->resume();
        insert(std::move(c.live), c.index);
        return true;
    }

    void closeOthers(int keep) {
        for (int i = 0; i < (int)list.size(); ++i)
            if (i != keep) close(i);
    }

    void closeRight(int from) {
        for (int i = from + 1; i < (int)list.size(); ++i) close(i);
    }

    void adopt(const TabHandoff& h, int index) {
        auto t = std::make_unique<Tab>();
        t->adopt(h);
        insert(std::move(t), index);
    }

    void insert(std::unique_ptr<Tab> t, int index) {
        if (hasMetrics) t->metrics(metrics, barWidth, family, points);
        if (width > 0) t->size(width, height);
        index = index < 0 || index > (int)list.size() ? (int)list.size() : index;
        if (index <= active && !list.empty()) ++active;
        list.insert(list.begin() + index, std::move(t));
        select(index);
    }

    bool take(int i, TabHandoff& h) {
        if (i < 0 || i >= (int)list.size() || !list[i]->detach(h)) return false;
        list[i]->active = false;
        list.erase(list.begin() + i);
        if (active > i || active >= (int)list.size()) active = (std::max)(0, active - 1);
        if (!list.empty()) select(active);
        return true;
    }

    void move(int from, int to) {
        if (from < 0 || from >= (int)list.size() || to < 0 || to >= (int)list.size() || from == to) return;
        Tab* shownTab = current();
        auto t = std::move(list[from]);
        list.erase(list.begin() + from);
        list.insert(list.begin() + to, std::move(t));
        for (int i = 0; i < (int)list.size(); ++i)
            if (list[i].get() == shownTab) active = i;
    }

    void select(int i) {
        if (i < 0 || i >= (int)list.size()) return;
        Tab* old = current();
        if (old) old->active = false;
        active = i;
        list[i]->active = true;
        list[i]->activity = false;
        if (focused && old != list[i].get()) {
            if (old) old->focus(false);
            list[i]->focus(true);
        }
    }

    void focus(bool on) {
        focused = on;
        if (Tab* t = current()) t->focus(on);
    }

    void cycle(int step) { if (!list.empty()) select((active + step + (int)list.size()) % (int)list.size()); }

    void close(int i) { if (i >= 0 && i < (int)list.size()) list[i]->park(); }

    void closeAll() {
        for (auto& t : list) t->close();
        for (auto& c : closed)
            if (c.live) c.live->close();
    }

    bool reap(int& lastExitCode) {
        bool removed = false;
        for (int i = (int)list.size() - 1; i >= 0; --i) {
            bool parked = list[i]->parked();
            if (!parked && !list[i]->exited) continue;
            if (!parked) lastExitCode = list[i]->exitCode;
            list[i]->active = false;
            ClosedTab c{ nullptr, list[i]->cwd(), i, secondsNow() };
            if (parked) c.live = std::move(list[i]);
            closed.push_back(std::move(c));
            list.erase(list.begin() + i);
            if (active > i || active >= (int)list.size()) active = (std::max)(0, active - 1);
            removed = true;
        }
        if (removed) trimClosed();
        if (removed && !list.empty()) select(active);
        return removed;
    }

    void trimClosed() {
        while ((int)closed.size() > MAX_CLOSED) {
            endInBackground(std::move(closed.front().live));
            closed.erase(closed.begin());
        }
        int live = 0;
        for (int i = (int)closed.size() - 1; i >= 0; --i)
            if (closed[i].live && ++live > MAX_PARKED) endInBackground(std::move(closed[i].live));
    }

    unsigned expireParked() {
        double now = secondsNow(), next = -1;
        for (ClosedTab& c : closed) {
            if (!c.live) continue;
            double left = c.closedAt + PARK_SECONDS - now;
            if (left <= 0) endInBackground(std::move(c.live));
            else if (next < 0 || left < next) next = left;
        }
        return next < 0 ? NO_LIMIT : (unsigned)(next * 1000) + 1;
    }

    void broadcastMetrics(const FontMetrics& m, int bar, const std::string& fam, float pts) {
        metrics = m;
        barWidth = bar;
        family = fam;
        points = pts;
        hasMetrics = true;
        for (auto& t : list) t->metrics(m, bar, fam, pts);
    }

    void broadcastSize(int w, int h) {
        if (w == width && h == height) return;
        width = w;
        height = h;
        for (auto& t : list) t->size(w, h);
    }

    std::vector<StripTab> stripTabs() {
        std::vector<StripTab> out;
        for (auto& t : list) out.push_back({ strip::codepoints(t->title()), t->activity.load() });
        return out;
    }

private:
    FontMetrics metrics;
    int barWidth = 12;
    std::string family;
    float points = 12;
    bool hasMetrics = false, focused = true;
    int width = 0, height = 0;
};
