#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

struct lua_State; // opaque, from lua.h -- avoids forcing the Lua C headers on every includer

enum class CmdKind { Builtin, LuaFn, External };

// A value crossing the Lua boundary: shell command args in, pipeline data
// out. Array holds either a plain scalar list or a list-of-records (each
// record then has type == Table). Table is one record: ordered string-keyed
// scalar/nested fields.
struct LuaValue {
    enum class Type { Nil, Bool, Number, String, Array, Table } type = Type::Nil;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<LuaValue> arr;
    std::vector<std::pair<std::string, LuaValue>> tbl;

    static LuaValue nil() { return LuaValue{}; }
    static LuaValue from_string(std::string v);
    static LuaValue from_lines(std::vector<std::string> lines);
};

namespace lua_env {

// Called by the shell's `sh()` Lua binding to run a raw shell-syntax command
// line from within Lua. Registered once at startup by main.cpp (which owns
// the Executor); breaks the lua_env <-> executor circular dependency.
using ShellCallback = std::function<int(const std::string& cmdline)>;
void set_shell_callback(ShellCallback cb);

lua_State* init();                 // luaL_newstate + openlibs + sh()/setenv() bindings
void close(lua_State* L);

std::string default_config_path(); // ~/.config/wisp/init.lua, honors $XDG_CONFIG_HOME
// Missing file: proceeds with empty config. Broken file: warns on stderr, doesn't abort.
void load_config(lua_State* L, const std::string& path);

// prompt() global, if defined; falls back to a plain default on absence or error.
std::string get_prompt(lua_State* L);

// Builtin > Lua global function > external ($PATH, checked lazily at exec time).
CmdKind resolve_command(lua_State* L, const std::string& name);

// Calls a Lua-function-command with the fixed convention `function(input, ...)`:
// input is the previous native stage's value (nil if has_input is false), argv
// is the plain shell-word arguments. Throws std::runtime_error on a Lua error.
LuaValue call_lua_function(lua_State* L, const std::string& name,
                            const std::vector<std::string>& argv,
                            const LuaValue& input, bool has_input);

// Row-per-line "key=value key=value" rendering for a value ending a native
// pipeline (terminal display) or crossing into an external process's stdin
// (serialization) -- deliberately not column-aligned, see README.
std::string render_value(const LuaValue& v);

} // namespace lua_env
