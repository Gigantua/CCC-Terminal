#pragma once
#include <string>
#include <vector>
#include "../shell/console.h"
#include "input.h"

struct MenuItem {
    std::string label;
    bool enabled = true;
    InputEvent::Kind action = InputEvent::TabNew;
    int value = 0;
};

struct Menu {
    bool open = false;
    int col = 0, row = 0, hover = -1;
    std::vector<MenuItem> items;

    int width() const {
        size_t w = 0;
        for (const MenuItem& i : items) w = (std::max)(w, i.label.size());
        return (int)w + 2;
    }

    int itemAt(int x, int y) const {
        int i = y - row;
        return open && x >= col && x < col + width() && i >= 0 && i < (int)items.size() && items[i].enabled ? i : -1;
    }

    static Menu forTab(int tab, int tabCount, int closedCount, int col) {
        Menu m;
        m.open = true;
        m.col = col;
        m.items = { { "New tab", true, InputEvent::TabNew, 0 },
                    { "Close tab", true, InputEvent::TabClose, tab },
                    { "Close other tabs", tabCount > 1, InputEvent::TabCloseOthers, tab },
                    { "Close tabs to the right", tab < tabCount - 1, InputEvent::TabCloseRight, tab },
                    { "Reopen closed tab", closedCount > 0, InputEvent::TabReopen, 0 } };
        return m;
    }

    void draw(Console::Frame& f) const {
        int w = width();
        for (int i = 0; i < (int)items.size() && row + i < f.rows; ++i) {
            vt::Attr a;
            a.flags = i == hover ? 0 : vt::REVERSE;
            if (!items[i].enabled) a.flags |= vt::DIM;
            std::string text = " " + items[i].label;
            text.resize((size_t)w, ' ');
            for (int x = 0; x < w && col + x < f.cols; ++x) {
                size_t at = (size_t)(row + i) * f.cols + col + x;
                f.cells[at] = (unsigned char)text[x];
                f.attrs[at] = a;
            }
        }
    }
};
