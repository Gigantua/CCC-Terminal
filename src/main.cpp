#include "../shell/cli.h"
#include "glyphs.h"
#include "gpu_vulkan.h"
#include "headless.h"
#include "settings.h"
#include "tabs.h"
#include "window.h"
#include "handoff.h"

struct Options {
    bool vsync = true, exitAfterRun = false, lineWrap = true, fontGiven = false, sizeGiven = false, tab = false, adopting = false;
    std::string run, font, shot;
    float points = 12;
    unsigned long adoptPid = 0;
    int adoptX = 0, adoptY = 0;
};

static std::string tabArgs(const Options& opt, bool first) {
    std::string a;
    if (!opt.lineWrap) a += " --nowrap";
    if (first && !opt.run.empty()) a += " --run " + quoteArg(opt.run);
    if (first && opt.exitAfterRun) a += " --exit";
    return a;
}

struct FontChoice {
    std::string family;
    float points = 12;
};

static void applyFont(const FontChoice& font, GlyphCache& glyphs, Gpu& gpu, Tabs& tabs, Window& win, unsigned dpi) {
    gpu.waitIdle();
    if (!glyphs.setFont(font.family, font.points * dpi / 72.0f)) return;
    glyphs.attach(gpu.atlas(), (uint32_t)(Gpu::ATLAS_BYTES / ((size_t)glyphs.tileTexels() * 4)));
    const FontMetrics& m = glyphs.metrics();
    int barWidth = Strip::scaled(12, dpi);
    win.cellW = m.cellW;
    win.cellH = m.cellH;
    win.scrollbarWidth = barWidth;
    tabs.broadcastMetrics(m, barWidth, glyphs.family(), font.points);
}

static void handleTabEvent(const InputEvent& e, Tabs& tabs, const Options& opt, Window& win) {
    switch (e.kind) {
    case InputEvent::TabMove: tabs.move(e.value, e.x); break;
    case InputEvent::TabTear: tearTab(tabs, win, e.value, e.x, e.y); break;
    case InputEvent::TabNew: tabs.open(tabArgs(opt, false)); break;
    case InputEvent::TabClose: tabs.close(e.value < 0 ? tabs.active : e.value); break;
    case InputEvent::TabCloseOthers: tabs.closeOthers(e.value); break;
    case InputEvent::TabCloseRight: tabs.closeRight(e.value); break;
    case InputEvent::TabReopen: if (!tabs.reopen(tabArgs(opt, false)) && e.value) tabs.open(tabArgs(opt, false)); break;
    case InputEvent::TabSelect: tabs.select(e.value); break;
    case InputEvent::TabCycle: tabs.cycle(e.value); break;
    case InputEvent::Focus: tabs.focus(e.value != 0); break;
    default: if (Tab* t = tabs.current()) t->input(e); break;
    }
}

static void serveRequests(Tabs& tabs, Window& win, InputQueue& inputs, FontChoice& font, bool& fontDirty) {
    for (auto& t : tabs.list) {
        TabRequests r = t->takeRequests();
        if (r.font) {
            if (!r.family.empty()) font.family = r.family;
            font.points = std::clamp(r.points, WindowHost::MIN_POINTS, WindowHost::MAX_POINTS);
            fontDirty = true;
        }
        for (const std::string& s : r.prints) writeStdout(s);
        if (r.bell) win.bell();
        if (r.clipboardSet) win.setClipboard(r.clipboard);
        if (r.clipboardWanted) win.readClipboard([&inputs](const std::string& text) { InputEvent e{ InputEvent::Clipboard }; e.text = text; inputs.push(e); requestFrame(); });
        for (const std::string& uri : r.opens) win.openUri(uri);
        for (auto& n : r.notifications) win.notify(n.first, n.second);
        if (r.cols > 0 && r.rows > 0 && t.get() == tabs.current())
            win.resizeClient(r.cols * win.cellW + win.scrollbarWidth, r.rows * win.cellH + win.strip().height);
    }
}

static GpuConstants stripConstants(GpuConstants k, const Strip& strip, const screen::View& view, const Window& win, unsigned dpi, int tabCount) {
    k.topInset = strip.height;
    k.stripX = strip.x0;
    k.stripTextTop = strip.textTop;
    k.stripTabTop = strip.tabTop;
    k.stripColor = strip::barColor(view.bg);
    k.buttonsX = strip.height ? strip.buttonsX : 0xFFFFFFFF;
    k.buttonW = (std::max)(strip.buttonW, 1);
    k.buttonHover = win.hoverButton + 1;
    k.buttonPressed = win.pressedButton + 1;
    k.iconColor = strip::mix(view.bg & 0xFFFFFF, strip::contrast(view.bg), 0.92f);
    k.iconHalf = Strip::scaled(5, dpi);
    k.iconStroke = (std::max)(1, (int)(dpi / 96));
    k.maximized = win.maximized();
    k.closeColor = vt::rgb(196, 43, 28);
    k.tabShape = (uint32_t)strip.tabCols | (uint32_t)(std::min)(tabCount, 255) << 8 | (uint32_t)Strip::scaled(6, dpi) << 16;
    return k;
}

static void renderLoop(Window& win, Tabs& tabs, Gpu& gpu, InputQueue& inputs, const Options& opt, FontChoice& font, const std::atomic<bool>& quit, int& exitCode) {
    GlyphCache glyphs;
    if (!glyphs.init()) return;
    unsigned appliedDpi = 0;
    bool blinking = true;
    std::string shownTitle;
    Console::Frame bar;
    int shownProgress = 0;
    while (!quit) {
        waitForChange(blinking, tabs.expireParked());
        if (quit) break;
        for (const TabHandoff& h : inputs.takeHandoffs()) tabs.adopt(h, h.index);
        for (const InputEvent& e : inputs.take()) handleTabEvent(e, tabs, opt, win);
        bool fontDirty = false;
        serveRequests(tabs, win, inputs, font, fontDirty);
        tabs.reap(exitCode);
        if (tabs.list.empty()) { win.tabCount = 0; win.requestClose(); break; }
        unsigned dpi = win.dpi;
        if (dpi != appliedDpi) { appliedDpi = dpi; fontDirty = true; }
        if (fontDirty) applyFont(font, glyphs, gpu, tabs, win, dpi);
        win.tabCount = (int)tabs.list.size();
        win.closedCount = (int)tabs.closed.size();
        Strip strip = win.strip();
        int w = win.width, h = win.height, termH = h - strip.height;
        if (w <= win.scrollbarWidth || termH <= 0) continue;
        tabs.broadcastSize(w, termH);
        Tab* tab = tabs.current();
        int progress = tab->progressState << 8 | tab->progressValue;
        if (progress != shownProgress) { shownProgress = progress; win.progress(tab->progressState, tab->progressValue); }
        tab->takeView();
        std::string title = tab->title();
        if (title != shownTitle) { win.setTitle(title); shownTitle = title; }
        const screen::View& view = tab->shown;
        if (!view.frame.cols) continue;
        win.setPointer(pointerNamed(view.frame.pointer));
        gpu.resize(w, h);
        gpu.beginFrame();
        bool blinkOn = blinkPhaseOn(secondsNow()), focused = win.focused;
        screen::Highlight highlight = view.highlight;
        highlight.hollowCursor = !focused;
        if (!blinkOn && focused) highlight.cursorRow = highlight.cursorCol = -1;
        strip::layout(bar, strip, tabs.stripTabs(), tabs.active, win.stripHover(), view.bg);
        size_t termCells = (size_t)view.frame.cols * view.frame.rows;
        GpuCell* cells = gpu.cells(termCells + bar.cols);
        Menu menu = win.menuView();
        Console::Frame withMenu;
        if (menu.open) { withMenu = view.frame; menu.draw(withMenu); }
        const Console::Frame& shownFrame = menu.open ? withMenu : view.frame;
        auto build = [&] {
            return screen::buildCells(shownFrame, highlight, view.fg, view.bg, glyphs, cells) &&
                   screen::buildCells(bar, screen::Highlight(), view.fg, view.bg, glyphs, cells + termCells);
        };
        if (!build()) {
            gpu.waitIdle();
            glyphs.reset();
            build();
        }
        GpuConstants k = stripConstants(screen::constants(view, glyphs.metrics(), w, termH, win.scrollbarWidth, blinkOn), strip, view, win, dpi, (int)tabs.list.size());
        k.stripOffset = (uint32_t)termCells;
        k.stripCols = bar.cols;
        k.pictureCount = screen::uploadPictures(view.frame, glyphs.metrics(), gpu);
        gpu.draw(k, opt.vsync, opt.shot);
        blinking = (focused && view.highlight.cursorRow >= 0) || view.frame.blinking;
    }
    gpu.waitIdle();
}

static int runWindow(const Options& opt) {
    InputQueue inputs;
    WindowSettings saved;
    bool remember = !opt.exitAfterRun && opt.shot.empty();
    if (remember) saved.load();
    FontChoice font{ opt.font, opt.points };
    if (!opt.fontGiven && !saved.font.empty()) font.family = saved.font;
    if (!opt.sizeGiven && saved.points > 0) font.points = std::clamp(saved.points, WindowHost::MIN_POINTS, WindowHost::MAX_POINTS);
    float pixels = font.points * Window::systemDpi() / 72.0f;
    int clientW = (int)(100 * pixels * 0.55f), clientH = (int)(30 * pixels * 1.3f);
    TabHandoff adopted;
    if (opt.adopting) {
        adopted = adoptedHandoff(opt.adoptPid, opt.adoptX, opt.adoptY);
        int w = saved.hasRect ? saved.rect.right - saved.rect.left : clientW, h = saved.hasRect ? saved.rect.bottom - saved.rect.top : clientH;
        int left = opt.adoptX - Strip::scaled(90, Window::systemDpi()), top = opt.adoptY - Strip::scaled(18, Window::systemDpi());
        saved.rect = { left, top, left + w, top + h };
        saved.hasRect = true;
        saved.maximized = false;
    }
    Window win;
    if (!win.create(clientW, clientH, inputs, saved)) return 1;
    Gpu gpu;
    if (!Window::loadVulkan() || !gpu.init(Window::surfaceExtensions(), Window::makeSurface, &win, win.width, win.height)) { win.alert("Vulkan is not available."); return 1; }
    Tabs tabs;
    if (opt.adopting) tabs.adopt(adopted, 0);
    else if (!tabs.open(tabArgs(opt, true))) { win.alert("Cannot start the shell process."); return 1; }
    std::atomic<bool> quit{ false };
    int exitCode = 0;
    std::thread render(renderLoop, std::ref(win), std::ref(tabs), std::ref(gpu), std::ref(inputs), std::cref(opt), std::ref(font), std::cref(quit), std::ref(exitCode));
    win.runMessageLoop();
    quit = true;
    requestFrame();
    win.finish(render);
    if (remember && win.savedPlacement(saved)) {
        saved.font = font.family;
        saved.points = font.points;
        saved.save();
    }
    tabs.closeAll();
    tabs.list.clear();
    tabs.closed.clear();
    return exitCode;
}


static int run(const std::vector<std::string>& args) {
    bool windowSwitch = false, shellSwitch = false;
    Options opt;
    for (size_t i = 1; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a.empty()) continue;
        auto next = [&] { return i + 1 < args.size() ? args[++i] : std::string(); };
        if (a == "--vsync") { opt.vsync = next() != "0"; windowSwitch = true; }
        else if (a == "--run") { opt.run = next(); windowSwitch = true; }
        else if (a == "--exit") { opt.exitAfterRun = true; windowSwitch = true; }
        else if (a == "--font") { opt.font = next(); opt.fontGiven = windowSwitch = true; }
        else if (a == "--size") { opt.points = (float)atof(next().c_str()); opt.sizeGiven = windowSwitch = true; }
        else if (a == "--nowrap") { opt.lineWrap = false; windowSwitch = true; }
        else if (a == "--shot") { opt.shot = next(); windowSwitch = true; }
        else if (a == "--tab") opt.tab = true;
        else if (a == "--adopt") {
            opt.adoptPid = strtoul(next().c_str(), nullptr, 10);
            opt.adoptX = atoi(next().c_str());
            opt.adoptY = atoi(next().c_str());
            opt.adopting = windowSwitch = true;
        }
        else if (a == "--headless") {
#ifndef _WIN32
            signal(SIGPIPE, SIG_IGN);
#endif
            return Headless().run(next());
        }
        else if (a[0] == '/' && a.size() > 1 && strchr("cCkK?", a[1])) shellSwitch = true;
    }
    if (opt.tab) {
        os::ignoreConsoleInterrupts();
        TabOptions t;
        t.lineWrap = opt.lineWrap;
        t.exitAfterRun = opt.exitAfterRun;
        t.run = opt.run;
        return TabProcess().run(t);
    }
    if (!windowSwitch && (shellSwitch || os::stdRedirected())) return ccc::cliMain();
    os::ignoreConsoleInterrupts();
    return runWindow(opt);
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i) args.push_back(ccc::U(argv[i]));
    return run(args);
}
#else
int main(int argc, char** argv) { return run(std::vector<std::string>(argv, argv + argc)); }
#endif
