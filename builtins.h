#pragma once

#include <array>
#include <cstdlib>
#include <dirent.h>
#include <set>
#include <string>
#include <unistd.h>

// Name resolution shared by every module that has to answer "what does this
// word run?" -- executor.cpp (implements the builtins), lua_env.cpp
// (classifies a word as Builtin before looking for a Lua function), and
// main.cpp (tab completion).

inline bool is_builtin_name(const std::string& name) {
    static constexpr std::array<const char*, 15> kBuiltins = {
        "cd", "exit", "export", "command", "jobs", "fg", "bg", "kill",
        "source", "echo", "disown", "wait", "pwd", "type", "pkg",
    };
    for (const char* b : kBuiltins)
        if (name == b) return true;
    return false;
}

// Every executable name on $PATH, scanned once and kept. Tab completion, the
// command-not-found hint and `type` all used to walk $PATH themselves,
// opendir-ing and stat-ing every entry on every keystroke; one shared cache
// makes those free after the first use. Reset when $PATH itself changes, so
// an `export PATH=...` still takes effect.
inline const std::set<std::string>& path_executables() {
    static std::string scanned_for;
    static std::set<std::string> cache;
    static bool primed = false;

    const char* env = std::getenv("PATH");
    std::string path = env ? env : "";
    if (primed && path == scanned_for) return cache;
    scanned_for = path;
    primed = true;
    cache.clear();

    size_t start = 0;
    while (start <= path.size()) {
        size_t colon = path.find(':', start);
        std::string dir = path.substr(start, colon == std::string::npos ? std::string::npos : colon - start);
        if (dir.empty()) dir = ".";
        if (DIR* d = opendir(dir.c_str())) {
            while (struct dirent* ent = readdir(d)) {
                if (ent->d_type == DT_DIR) continue; // cheap reject, no stat needed
                std::string name = ent->d_name;
                if (name == "." || name == "..") continue;
                if (access((dir + "/" + name).c_str(), X_OK) == 0) cache.insert(std::move(name));
            }
            closedir(d);
        }
        if (colon == std::string::npos) break;
        start = colon + 1;
    }
    return cache;
}

// Full path of `name` on $PATH, or "" -- what `type` reports.
inline std::string which(const std::string& name) {
    const char* env = std::getenv("PATH");
    if (!env || name.empty()) return "";
    std::string path(env);
    size_t start = 0;
    while (start <= path.size()) {
        size_t colon = path.find(':', start);
        std::string dir = path.substr(start, colon == std::string::npos ? std::string::npos : colon - start);
        if (dir.empty()) dir = ".";
        std::string full = dir + "/" + name;
        if (access(full.c_str(), X_OK) == 0) return full;
        if (colon == std::string::npos) break;
        start = colon + 1;
    }
    return "";
}
