#pragma once
#include "util.h"
#include <algorithm>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ccc {

class DirIndex {
    struct Dir {
        std::string path;
        std::unique_ptr<os::DirWatch> watch;
        bool stale = true;
        std::unordered_set<std::string> files;
    };
    std::unordered_map<std::string, std::unique_ptr<Dir>> dirs;
    std::mutex lock;

    Dir& entry(const std::string& dir) {
        std::string path = withSlash(dir);
        auto& d = dirs[os::foldName(path)];
        if (!d) { d = std::make_unique<Dir>(); d->path = path; }
        return *d;
    }

    static void unwatch(Dir& d) {
        d.watch.reset();
        d.stale = true;
    }

    static void markChanged(std::vector<Dir*>& ds) {
        for (Dir* d : ds)
            if (d->watch && !d->stale && d->watch->changed()) unwatch(*d);
    }

    static void load(Dir& d) {
        if (!d.watch) {
            d.watch = std::make_unique<os::DirWatch>(d.path);
            if (!d.watch->active()) d.watch.reset();
        }
        d.files.clear();
        for (const os::FileInfo& f : os::listFiles(d.path + "*"))
            if (!(f.attr & os::ATTR_DIRECTORY)) d.files.insert(os::foldName(f.name));
        d.stale = d.watch == nullptr;
    }

public:
    using Accept = std::function<bool(size_t dirIndex, const std::string& ext)>;

    bool find(const std::vector<std::string>& list, const std::string& name, bool bare, const std::vector<std::string>& exts,
              const Accept& accept, size_t& dirIndex, std::string& ext) {
        if constexpr (!os::INDEX_PATH_DIRS) return probe(list, name, bare, exts, accept, dirIndex, ext);
        std::lock_guard<std::mutex> guard(lock);
        std::vector<Dir*> ds;
        ds.reserve(list.size());
        for (auto& dir : list) ds.push_back(&entry(dir));
        markChanged(ds);
        std::string key = os::foldName(name);
        std::vector<std::string> keyedExts;
        for (auto& e : exts) keyedExts.push_back(os::foldName(e));
        for (size_t i = 0; i < ds.size(); ++i) {
            Dir& d = *ds[i];
            if (d.stale) {
                std::vector<Dir*> pending;
                for (size_t k = i; k < ds.size(); ++k) if (ds[k]->stale && std::find(pending.begin(), pending.end(), ds[k]) == pending.end()) pending.push_back(ds[k]);
                os::forEachParallel(pending.begin(), pending.end(), [](Dir* p) { load(*p); });
            }
            if (bare && d.files.contains(key) && accept(i, "")) { dirIndex = i; ext.clear(); return true; }
            for (size_t e = 0; e < keyedExts.size(); ++e)
                if (d.files.contains(key + keyedExts[e]) && accept(i, exts[e])) { dirIndex = i; ext = exts[e]; return true; }
        }
        return false;
    }

    static bool probe(const std::vector<std::string>& list, const std::string& name, bool bare, const std::vector<std::string>& exts,
                      const Accept& accept, size_t& dirIndex, std::string& ext) {
        for (size_t i = 0; i < list.size(); ++i) {
            std::string base = withSlash(list[i]) + name;
            if (bare && isFile(base) && accept(i, "")) { dirIndex = i; ext.clear(); return true; }
            for (auto& e : exts)
                if (isFile(base + e) && accept(i, e)) { dirIndex = i; ext = e; return true; }
        }
        return false;
    }
};

}
