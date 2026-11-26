#pragma once
#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include "vt.h"

class ExtraCursors {
public:
    enum Shape { NONE = 0, BLOCK = 1, BEAM = 2, UNDERLINE = 3, FOLLOW = 29 };
    static constexpr uint32_t REVERSE = 0xFFFFFFFE;
    std::map<int, int> shapes;
    uint32_t textColor = vt::COLOR_DEFAULT, cursorColor = vt::COLOR_DEFAULT;

    std::string command(const uint32_t* p, int n, uint32_t sub, int cols, int rows) {
        if (n == 0) return "\x1b[>1;2;3;29;30;40;100;101 q";
        lastCols = cols;
        int shape = (int)p[0];
        std::vector<std::pair<int, int>> groups;
        for (int i = 1; i < n;) {
            int j = i + 1;
            while (j < n && (sub >> j & 1)) ++j;
            groups.push_back({ i, j });
            i = j;
        }
        if (shape == 100) return listed();
        if (shape == 101) return "\x1b[>101;30:" + colorSpec(textColor) + ";40:" + colorSpec(cursorColor) + " q";
        if (shape == 30 || shape == 40) {
            if (!groups.empty()) (shape == 30 ? textColor : cursorColor) = colorOf(p + groups[0].first, groups[0].second - groups[0].first);
            return "";
        }
        if (shape != NONE && shape != BLOCK && shape != BEAM && shape != UNDERLINE && shape != FOLLOW) return "";
        for (auto [from, to] : groups) {
            const uint32_t* q = p + from;
            int count = to - from;
            if (q[0] == 2)
                for (int k = 1; k + 1 < count; k += 2) set((int)q[k] - 1, (int)q[k + 1] - 1, shape, cols, rows);
            else if (q[0] == 4) {
                if (count == 1) fill(0, 0, rows - 1, cols - 1, shape, cols, rows);
                for (int k = 1; k + 3 < count; k += 4) fill((int)q[k] - 1, (int)q[k + 1] - 1, (int)q[k + 2] - 1, (int)q[k + 3] - 1, shape, cols, rows);
            }
        }
        return "";
    }

    void clear() { shapes.clear(); }

private:
    void set(int y, int x, int shape, int cols, int rows) {
        if (y < 0 || x < 0 || y >= rows || x >= cols) return;
        if (shape == NONE) shapes.erase(y * cols + x);
        else shapes[y * cols + x] = shape;
    }

    void fill(int top, int left, int bottom, int right, int shape, int cols, int rows) {
        for (int y = (std::max)(top, 0); y <= (std::min)(bottom, rows - 1); ++y)
            for (int x = (std::max)(left, 0); x <= (std::min)(right, cols - 1); ++x) set(y, x, shape, cols, rows);
    }

    static uint32_t colorOf(const uint32_t* q, int n) {
        if (q[0] == 1) return REVERSE;
        if (q[0] == 2 && n >= 4) return vt::rgb(q[1], q[2], q[3]);
        if (q[0] == 5 && n >= 2) return vt::COLOR_INDEXED | (q[1] & 255);
        return vt::COLOR_DEFAULT;
    }

    static std::string colorSpec(uint32_t c) {
        if (c == REVERSE) return "1";
        if (c & vt::COLOR_DEFAULT) return "0";
        if (c & vt::COLOR_INDEXED) return "5:" + std::to_string(c & 255);
        return "2:" + std::to_string(c & 255) + ":" + std::to_string(c >> 8 & 255) + ":" + std::to_string(c >> 16 & 255);
    }

    std::string listed() const {
        std::string out = "\x1b[>100";
        for (auto [cell, shape] : shapes) out += ";" + std::to_string(shape) + ":2:" + std::to_string(cell / lastCols + 1) + ":" + std::to_string(cell % lastCols + 1);
        return out + " q";
    }

    int lastCols = 1;
};
