#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <set>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include "builtins.h"
#include "executor.h"
#include "linenoise.h"
#include "lua_env.h"

#define WISP_VERSION "3.2.1"

namespace {

lua_State* g_L = nullptr; // for the completion callback only

const char kUsage[] =
    "usage: wisp [options] [script]\n"
    "\n"
    "  -c <line>   run a shell command line (a leading ':' makes it Lua)\n"
    "  -e <chunk>  run a Lua chunk\n"
    "  -f <file>   run a Lua script file\n"
    "  --version   print the version\n"
    "  --help      print this message\n"
    "\n"
    "With no arguments, starts the interactive shell.\n"
    "Config: $XDG_CONFIG_HOME/wisp/init.lua (default ~/.config/wisp/init.lua)\n";

std::string history_path() {
    const char* home = std::getenv("HOME");
    return home ? std::string(home) + "/.wisp_history" : std::string();
}

bool starts_with(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool is_cmd_delim(char c) { return c == '|' || c == ';' || c == '&'; }

// True when the token starting at `tok_start` sits in command position:
// start of line, immediately after | / ; / & (ignoring spaces), or right
// after a `command` prefix at command position.
bool is_command_position(const std::string& s, size_t tok_start) {
    size_t j = tok_start;
    while (j > 0 && (s[j - 1] == ' ' || s[j - 1] == '\t')) --j;
    if (j == 0 || is_cmd_delim(s[j - 1])) return true;

    size_t word_end = j;
    while (j > 0 && s[j - 1] != ' ' && s[j - 1] != '\t' && !is_cmd_delim(s[j - 1])) --j;
    if (word_end - j == 7 && s.compare(j, 7, "command") == 0) {
        while (j > 0 && (s[j - 1] == ' ' || s[j - 1] == '\t')) --j;
        return j == 0 || is_cmd_delim(s[j - 1]);
    }
    return false;
}

char* history_hint(const char* buf, int* color, int* bold) {
    if (!buf || !*buf) return nullptr;
    size_t blen = std::strlen(buf);
    // history[count - 1] is the live line being edited, so start at count - 2.
    for (int i = linenoiseHistoryCount() - 2; i >= 0; --i) {
        const char* entry = linenoiseHistoryGet(i);
        if (entry && std::strncmp(entry, buf, blen) == 0 && entry[blen] != '\0') {
            *color = 90; // dim gray
            *bold = 0;
            return strdup(entry + blen);
        }
    }
    return nullptr;
}

void completion(const char* buf, linenoiseCompletions* lc) {
    std::string s(buf);
    bool lua_mode = !s.empty() && s[0] == ':';

    if (lua_mode) {
        std::string prefix = s.substr(1);
        size_t last_dot = prefix.find_last_of('.');
        std::string before = s.substr(0, s.size() - prefix.size());
        std::string table_path;
        if (last_dot != std::string::npos) {
            table_path = prefix.substr(0, last_dot);
            before += prefix.substr(0, last_dot + 1);
            prefix = prefix.substr(last_dot + 1);
        }
        if (prefix.empty() && table_path.empty()) return;

        if (g_L) {
            int top = lua_gettop(g_L);
            lua_pushglobaltable(g_L);
            bool ok = true;
            if (!table_path.empty()) {
                size_t start = 0;
                while (start <= table_path.size()) {
                    size_t dot = table_path.find('.', start);
                    std::string seg =
                        table_path.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
                    if (seg.empty()) {
                        ok = false;
                        break;
                    }
                    lua_getfield(g_L, -1, seg.c_str());
                    if (!lua_istable(g_L, -1)) {
                        ok = false;
                        break;
                    }
                    if (dot == std::string::npos) break;
                    start = dot + 1;
                }
            }
            if (ok) {
                std::vector<std::string> matches;
                lua_pushnil(g_L);
                while (lua_next(g_L, -2) != 0) {
                    if (lua_type(g_L, -2) == LUA_TSTRING) {
                        const char* name = lua_tostring(g_L, -2);
                        if (name && starts_with(name, prefix)) matches.emplace_back(name);
                    }
                    lua_pop(g_L, 1);
                }
                std::sort(matches.begin(), matches.end());
                for (const auto& m : matches) {
                    linenoiseAddCompletion(lc, (before + m).c_str());
                }
            }
            lua_settop(g_L, top);
        }
        return;
    }

    size_t tok_start = s.size();
    while (tok_start > 0 && s[tok_start - 1] != ' ' && s[tok_start - 1] != '\t' &&
           !is_cmd_delim(s[tok_start - 1])) {
        --tok_start;
    }
    bool cmd_pos = is_command_position(s, tok_start);
    std::string prefix = s.substr(tok_start);
    std::string before = s.substr(0, tok_start);

    if (prefix.empty() && cmd_pos) return;

    std::set<std::string> seen;

    auto add_if_new = [&](const std::string& name) {
        if (seen.insert(name).second)
            linenoiseAddCompletion(lc, (before + name).c_str());
    };

    if (cmd_pos) {
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

        // Cached, so this costs nothing after the first Tab. The set is
        // sorted, so a prefix scan can stop at the first name past it.
        const auto& execs = path_executables();
        for (auto it = execs.lower_bound(prefix); it != execs.end(); ++it) {
            if (!starts_with(*it, prefix)) break;
            add_if_new(*it);
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
        if (starts_with(opendir_path, "~/")) {
            if (const char* home = std::getenv("HOME"); home && *home) {
                opendir_path = std::string(home) + opendir_path.substr(1);
            }
        }
        if (DIR* d = opendir(opendir_path.c_str())) {
            std::vector<std::string> matches;
            struct dirent* ent;
            while ((ent = readdir(d)) != nullptr) {
                std::string name = ent->d_name;
                if (name == "." || name == "..") continue;
                if (name[0] == '.' && (base.empty() || base[0] != '.')) continue;
                if (!starts_with(name, base)) continue;
                std::string full = (slash != std::string::npos) ? dir + name : name;
                std::string check_path = (!opendir_path.empty() && opendir_path.back() == '/')
                                             ? (opendir_path + name)
                                             : (opendir_path + "/" + name);
                struct stat st{};
                if (stat(check_path.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
                    full += "/";
                matches.push_back(std::move(full));
            }
            closedir(d);
            std::sort(matches.begin(), matches.end());
            for (const auto& m : matches) add_if_new(m);
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

    // Guarded only across the call, never across the `>>> ` continuation
    // prompt above: a hook armed while no Lua is running would fire on
    // whatever the user typed next instead.
    int rc;
    {
        lua_env::InterruptGuard guard(L);
        rc = lua_pcall(L, 0, LUA_MULTRET, 0);
    }
    if (rc != LUA_OK) {
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

// Loads and runs a Lua file: `-f script.lua [args...]`, or a shebang'd script
// wisp was invoked on directly. Populates Lua's standard `arg` table.
static int run_script(lua_State* L, const char* path, int argc, char** argv, int arg_start) {
    lua_newtable(L);
    lua_pushstring(L, path);
    lua_rawseti(L, -2, 0);
    for (int i = arg_start; i < argc; ++i) {
        lua_pushstring(L, argv[i]);
        lua_rawseti(L, -2, i - arg_start + 1);
    }
    lua_setglobal(L, "arg");

    if (luaL_loadfile(L, path) != LUA_OK) {
        std::fprintf(stderr, "wisp: %s: %s\n", path, lua_tostring(L, -1));
        lua_settop(L, 0);
        return 1;
    }
    Executor exec(L);
    int status = lua_pcall(L, 0, LUA_MULTRET, 0);
    if (status != LUA_OK) std::fprintf(stderr, "wisp: %s\n", lua_tostring(L, -1));
    exec.reap_background();
    lua_settop(L, 0);
    return status != LUA_OK ? 1 : 0;
}

int main(int argc, char** argv) {
    std::signal(SIGPIPE, SIG_IGN);

    // Answered before Lua is even started: these have to work when something
    // is merely probing what it just found on $PATH.
    if (argc >= 2) {
        if (std::strcmp(argv[1], "--version") == 0 || std::strcmp(argv[1], "-v") == 0) {
            std::printf("wisp %s\n", WISP_VERSION);
            return 0;
        }
        if (std::strcmp(argv[1], "--help") == 0 || std::strcmp(argv[1], "-h") == 0) {
            std::fputs(kUsage, stdout);
            return 0;
        }
    }

    lua_State* L = lua_env::init();
    g_L = L;
    lua_env::load_config(L, lua_env::default_config_path());

    struct Closer {
        lua_State* L;
        ~Closer() { lua_env::close(L); }
    } closer{L};

    const char* flag = argc >= 2 ? argv[1] : "";
    bool takes_arg = std::strcmp(flag, "-c") == 0 || std::strcmp(flag, "-e") == 0 ||
                     std::strcmp(flag, "-f") == 0;
    if (takes_arg && argc < 3) {
        std::fprintf(stderr, "wisp: %s requires an argument\n", flag);
        return 1;
    }

    // -c is shell syntax, the way everything that spawns $SHELL expects; a
    // leading ':' still means Lua, exactly as at the prompt. -e is Lua
    // outright.
    if (std::strcmp(flag, "-c") == 0) {
        Executor exec(L);
        int status = argv[2][0] == ':' ? run_lua_sigil(L, argv[2] + 1) : exec.run_line_str(argv[2]);
        exec.reap_background();
        return status;
    }
    if (std::strcmp(flag, "-e") == 0) return run_lua_sigil(L, argv[2]);
    if (std::strcmp(flag, "-f") == 0) return run_script(L, argv[2], argc, argv, 3);
    if (argc >= 2) return run_script(L, argv[1], argc, argv, 2);

    Executor exec(L);
    exec.init_job_control();
    exec.set_history_callback([]() {
        std::vector<std::string> out;
        int n = linenoiseHistoryCount();
        out.reserve(n > 0 ? static_cast<size_t>(n) : 0);
        for (int i = 0; i < n; ++i) {
            if (const char* e = linenoiseHistoryGet(i)) out.emplace_back(e);
        }
        return out;
    });

    linenoiseSetCompletionCallback(completion);
    linenoiseSetHintsCallback(history_hint);
    linenoiseSetFreeHintsCallback(std::free);
    linenoiseSetMultiLine(1); // long lines wrap instead of scrolling over themselves
    linenoiseHistorySetMaxLen(1000);
    std::string hist = history_path();
    if (!hist.empty()) linenoiseHistoryLoad(hist.c_str());

    for (;;) {
        std::string prompt = lua_env::get_prompt(L);
        errno = 0;
        char* raw = linenoise(prompt.c_str());
        if (!raw) {
            // linenoise reports Ctrl-C the same way it reports EOF, minus
            // errno. Abandoning the line is not a reason to end the session.
            if (errno == EAGAIN) {
                exec.set_status(130);
                continue;
            }
            break; // Ctrl-D / EOF
        }

        std::string line(raw);
        linenoiseFree(raw);

        if (!line.empty()) {
            linenoiseHistoryAdd(line.c_str());
            if (!hist.empty()) linenoiseHistorySave(hist.c_str());
            if (line[0] == ':') exec.set_status(run_lua_sigil(L, line.substr(1)));
            else exec.run_line_str(line);
        }
        exec.reap_background();
    }

    return 0;
}
