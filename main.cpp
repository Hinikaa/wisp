#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <set>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include "executor.h"
#include "linenoise.h"
#include "lua_env.h"

namespace {

lua_State* g_L = nullptr; // for the completion callback only

std::string history_path() {
    const char* home = std::getenv("HOME");
    return home ? std::string(home) + "/.wisp_history" : std::string();
}

bool starts_with(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

// v1 completion
void completion(const char* buf, linenoiseCompletions* lc) {
    std::string s(buf);
    bool lua_mode = !s.empty() && s[0] == ':';

    if (lua_mode) {
        std::string prefix = s.substr(1);
        size_t last_dot = prefix.find_last_of('.');
        std::string before = s.substr(0, s.size() - prefix.size());
        if (last_dot != std::string::npos) {
            before += prefix.substr(0, last_dot + 1);
            prefix = prefix.substr(last_dot + 1);
        }
        if (prefix.empty()) return;

        if (g_L) {
            lua_pushglobaltable(g_L);
            lua_pushnil(g_L);
            while (lua_next(g_L, -2) != 0) {
                if (lua_type(g_L, -2) == LUA_TSTRING) {
                    const char* name = lua_tostring(g_L, -2);
                    if (name && starts_with(name, prefix))
                        linenoiseAddCompletion(lc, (before + name).c_str());
                }
                lua_pop(g_L, 1);
            }
            lua_pop(g_L, 1);
        }
        return;
    }

    size_t last_space = s.find_last_of(' ');
    bool first_word = (last_space == std::string::npos);
    std::string prefix = first_word ? s : s.substr(last_space + 1);
    std::string before = first_word ? "" : s.substr(0, last_space + 1);

    if (prefix.empty() && first_word) return;

    std::set<std::string> seen;

    auto add_if_new = [&](const std::string& name) {
        if (seen.insert(name).second)
            linenoiseAddCompletion(lc, (before + name).c_str());
    };

    if (first_word) {
        static const char* kBuiltins[] = {"cd", "exit", "export", "command", "jobs", "fg", "bg", "kill", "source", "echo", "disown", "wait", "pwd", "type"};
        for (const char* b : kBuiltins)
            if (starts_with(b, prefix)) add_if_new(b);

        if (g_L) {
            lua_pushglobaltable(g_L);
            lua_pushnil(g_L);
            while (lua_next(g_L, -2) != 0) {
                if (lua_type(g_L, -2) == LUA_TSTRING && lua_isfunction(g_L, -1)) {
                    const char* name = lua_tostring(g_L, -2);
                    if (name && starts_with(name, prefix)) add_if_new(name);
                }
                lua_pop(g_L, 1);
            }
            lua_pop(g_L, 1);
        }

        const char* path_env = std::getenv("PATH");
        if (path_env) {
            std::string path(path_env);
            size_t start = 0;
            while (start <= path.size()) {
                size_t colon = path.find(':', start);
                std::string dir = path.substr(start, colon == std::string::npos ? std::string::npos : colon - start);
                if (dir.empty()) dir = ".";
                DIR* d = opendir(dir.c_str());
                if (d) {
                    struct dirent* ent;
                    while ((ent = readdir(d)) != nullptr) {
                        std::string name = ent->d_name;
                        if (name == "." || name == "..") continue;
                        if (!starts_with(name, prefix)) continue;
                        std::string full = dir + "/" + name;
                        struct stat st{};
                        if (stat(full.c_str(), &st) == 0 && (st.st_mode & S_IXUSR))
                            add_if_new(name);
                    }
                    closedir(d);
                }
                if (colon == std::string::npos) break;
                start = colon + 1;
            }
        }
    } else {
        std::string dir = ".", base = prefix;
        size_t slash = prefix.find_last_of('/');
        if (slash != std::string::npos) {
            dir = prefix.substr(0, slash + 1);
            base = prefix.substr(slash + 1);
            if (dir.empty()) dir = "/";
        }
        std::string opendir_path = (slash != std::string::npos) ? dir : ".";
        if (DIR* d = opendir(opendir_path.c_str())) {
            struct dirent* ent;
            while ((ent = readdir(d)) != nullptr) {
                std::string name = ent->d_name;
                if (name == "." || name == "..") continue;
                if (!prefix.empty() && name[0] == '.' && base[0] != '.') continue;
                if (!starts_with(name, base)) continue;
                std::string full = (slash != std::string::npos) ? dir + name : name;
                std::string complete = before + full;
                std::string check_path = opendir_path + "/" + name;
                struct stat st{};
                if (stat(check_path.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
                    complete += "/";
                add_if_new(complete);
            }
            closedir(d);
        }
    }
}

} // namespace

// Mirrors lua.c's own standalone REPL: try the input as an expression first
// (prefixed with "return ") so bare `:1+1` prints 2; fall back to loading it
// as a plain statement/chunk. An incomplete chunk (error message ending in
// "<eof>") prompts for another line and retries, rather than erroring --
// this is lua.c's proven multi-line continuation trick, not a new one.
int run_lua_sigil(lua_State* L, std::string chunk) {
    int top_before = lua_gettop(L);

    if (luaL_loadstring(L, ("return " + chunk).c_str()) != LUA_OK) {
        lua_settop(L, top_before);
        for (;;) {
            int status = luaL_loadstring(L, chunk.c_str());
            if (status == LUA_OK) break;
            size_t len = 0;
            const char* msg = lua_tolstring(L, -1, &len);
            bool incomplete = msg && len >= 5 && std::string(msg + len - 5, 5) == "<eof>";
            if (!incomplete) {
                std::fprintf(stderr, "wisp: lua: %s\n", msg ? msg : "syntax error");
                lua_settop(L, top_before);
                return 1;
            }
            lua_settop(L, top_before);
            char* cont = linenoise(">>> ");
            if (!cont) return 1;
            chunk += "\n";
            chunk += cont;
            linenoiseFree(cont);
        }
    }

    if (lua_pcall(L, 0, LUA_MULTRET, 0) != LUA_OK) {
        std::fprintf(stderr, "wisp: lua: %s\n", lua_tostring(L, -1));
        lua_settop(L, top_before);
        return 1;
    }
    int nret = lua_gettop(L) - top_before;
    for (int i = 1; i <= nret; ++i) {
        const char* s = luaL_tolstring(L, top_before + i, nullptr);
        if (i > 1) std::fputc('\t', stdout);
        std::fputs(s ? s : "", stdout);
        lua_pop(L, 1);
    }
    if (nret > 0) std::fputc('\n', stdout);
    lua_settop(L, top_before);
    return 0;
}

int main(int argc, char** argv) {
    lua_State* L = lua_env::init();
    g_L = L;
    lua_env::load_config(L, lua_env::default_config_path());

    if (argc >= 3 && std::strcmp(argv[1], "-c") == 0) {
        int status = run_lua_sigil(L, argv[2]);
        lua_env::close(L);
        return status;
    }

    if (argc >= 2 && std::strcmp(argv[1], "-c") == 0) {
        std::fprintf(stderr, "wisp: -c requires an argument\n");
        lua_env::close(L);
        return 1;
    }

    if (argc >= 3 && std::strcmp(argv[1], "-f") == 0) {
        const char* path = argv[2];
        if (luaL_loadfile(L, path) != LUA_OK) {
            std::fprintf(stderr, "wisp: %s: %s\n", path, lua_tostring(L, -1));
            lua_settop(L, 0);
            lua_env::close(L);
            return 1;
        }
        Executor exec(L);
        exec.init_job_control();
        int status = lua_pcall(L, 0, LUA_MULTRET, 0);
        if (status != LUA_OK) std::fprintf(stderr, "wisp: %s\n", lua_tostring(L, -1));
        exec.reap_background();
        lua_settop(L, 0);
        lua_env::close(L);
        return status != LUA_OK ? 1 : 0;
    }

    if (argc >= 2 && std::strcmp(argv[1], "-f") == 0) {
        std::fprintf(stderr, "wisp: -f requires a filename\n");
        lua_env::close(L);
        return 1;
    }

    if (argc >= 2) {
        // Shebang: wisp invoked directly on a script file (#!/usr/bin/env wisp)
        const char* path = argv[1];
        if (luaL_loadfile(L, path) != LUA_OK) {
            std::fprintf(stderr, "wisp: %s: %s\n", path, lua_tostring(L, -1));
            lua_settop(L, 0);
            lua_env::close(L);
            return 1;
        }
        Executor exec(L);
        exec.init_job_control();
        int status = lua_pcall(L, 0, LUA_MULTRET, 0);
        if (status != LUA_OK) std::fprintf(stderr, "wisp: %s\n", lua_tostring(L, -1));
        exec.reap_background();
        lua_settop(L, 0);
        lua_env::close(L);
        return status != LUA_OK ? 1 : 0;
    }

    Executor exec(L);
    exec.init_job_control();

    linenoiseSetCompletionCallback(completion);
    linenoiseHistorySetMaxLen(1000);
    std::string hist = history_path();
    if (!hist.empty()) linenoiseHistoryLoad(hist.c_str());

    for (;;) {
        std::string prompt = lua_env::get_prompt(L);
        char* raw = linenoise(prompt.c_str());
        if (!raw) break; // Ctrl-D / EOF

        std::string line(raw);
        linenoiseFree(raw);

        if (!line.empty()) {
            linenoiseHistoryAdd(line.c_str());
            if (!hist.empty()) linenoiseHistorySave(hist.c_str());
            if (line[0] == ':') run_lua_sigil(L, line.substr(1));
            else exec.run_line_str(line);
        }
        exec.reap_background();
    }

    lua_env::close(L);
    return 0;
}
