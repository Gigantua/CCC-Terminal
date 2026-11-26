#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

struct Bitmap {
    int width = 0, height = 0;
    std::vector<uint32_t> pixels;
    uint64_t key = 0;

    static uint64_t nextKey() {
        static std::atomic<uint64_t> serial{ 0 };
        return ++serial;
    }
};

struct Picture {
    std::shared_ptr<const Bitmap> bitmap;
    int col = 0, row = 0, offX = 0, offY = 0;
    int cols = 0, rows = 0;
    int srcX = 0, srcY = 0, srcW = 0, srcH = 0;
    int z = 0;
    int destW = 0, destH = 0;
};
