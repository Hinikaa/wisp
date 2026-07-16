#include "lua_env.h"

#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <sys/stat.h>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

#include "builtins.h"

LuaValue LuaValue::from_string(std::string v) {
    LuaValue r;
    r.type = Type::String;
    r.s = std::move(v);
    return r;
}

LuaValue LuaValue::from_lines(std::vector<std::string> lines) {
    LuaValue r;
    r.type = Type::Array;
    r.arr.reserve(lines.size());
    for (auto& l : lines) r.arr.push_back(from_string(std::move(l)));
    return r;
}

namespace {

lua_env::ShellCallback g_shell_cb;

int l_sh(lua_State* L) {
    const char* cmdline = luaL_checkstring(L, 1);
    int rc = g_shell_cb ? g_shell_cb(cmdline) : 127;
    lua_pushinteger(L, rc);
    return 1;
}

int l_setenv(lua_State* L) {
    const char* k = luaL_checkstring(L, 1);
    const char* v = luaL_checkstring(L, 2);
    setenv(k, v, 1);
    return 0;
}

// A table is treated as an Array (plain list, or list-of-records) when every
// key is exactly the integer sequence 1..len -- i.e. lua_rawlen()'s count
// accounts for all of the table's entries. Anything else (string keys) is a
// single record (Table). Empty {} is treated as an empty record; arbitrary
// but harmless, and documented.
bool is_array_table(lua_State* L, int idx) {
    idx = lua_absindex(L, idx);
    lua_Integer len = static_cast<lua_Integer>(lua_rawlen(L, idx));
    if (len == 0) return false;
    lua_Integer count = 0;
    lua_pushnil(L);
    while (lua_next(L, idx) != 0) {
        ++count;
        lua_pop(L, 1);
    }
    return count == len;
}

LuaValue pull_luavalue(lua_State* L, int idx) {
    idx = lua_absindex(L, idx);
    LuaValue v;
    switch (lua_type(L, idx)) {
        case LUA_TNIL:
            v.type = LuaValue::Type::Nil;
            break;
        case LUA_TBOOLEAN:
            v.type = LuaValue::Type::Bool;
            v.b = lua_toboolean(L, idx);
            break;
        case LUA_TNUMBER:
            v.type = LuaValue::Type::Number;
            v.n = lua_tonumber(L, idx);
            break;
        case LUA_TSTRING: {
            size_t len = 0;
            const char* s = lua_tolstring(L, idx, &len);
            v.type = LuaValue::Type::String;
            v.s.assign(s, len);
            break;
        }
        case LUA_TTABLE: {
            if (is_array_table(L, idx)) {
                v.type = LuaValue::Type::Array;
                lua_Integer len = static_cast<lua_Integer>(lua_rawlen(L, idx));
                for (lua_Integer i = 1; i <= len; ++i) {
                    lua_rawgeti(L, idx, i);
                    v.arr.push_back(pull_luavalue(L, -1));
                    lua_pop(L, 1);
                }
            } else {
                v.type = LuaValue::Type::Table;
                lua_pushnil(L);
                while (lua_next(L, idx) != 0) {
                    // Skip non-string keys: calling lua_tostring on a numeric
                    // key would coerce that stack slot in place and corrupt
                    // lua_next's traversal. Records are string-keyed in v1.
                    if (lua_type(L, -2) == LUA_TSTRING) {
                        size_t klen = 0;
                        const char* k = lua_tolstring(L, -2, &klen);
                        v.tbl.emplace_back(std::string(k, klen), pull_luavalue(L, -1));
                    }
                    lua_pop(L, 1);
                }
            }
            break;
        }
        default:
            v.type = LuaValue::Type::Nil; // function/userdata/thread: not representable
            break;
    }
    return v;
}

void push_luavalue(lua_State* L, const LuaValue& v) {
    switch (v.type) {
        case LuaValue::Type::Nil:
            lua_pushnil(L);
            break;
        case LuaValue::Type::Bool:
            lua_pushboolean(L, v.b);
            break;
        case LuaValue::Type::Number:
            lua_pushnumber(L, v.n);
            break;
        case LuaValue::Type::String:
            lua_pushlstring(L, v.s.data(), v.s.size());
            break;
        case LuaValue::Type::Array:
            lua_createtable(L, static_cast<int>(v.arr.size()), 0);
            for (size_t i = 0; i < v.arr.size(); ++i) {
                push_luavalue(L, v.arr[i]);
                lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
            }
            break;
        case LuaValue::Type::Table:
            lua_createtable(L, 0, static_cast<int>(v.tbl.size()));
            for (auto& kv : v.tbl) {
                push_luavalue(L, kv.second);
                lua_setfield(L, -2, kv.first.c_str());
            }
            break;
    }
}

void append_scalar(std::string& out, const LuaValue& v) {
    switch (v.type) {
        case LuaValue::Type::Nil:
            out += "nil";
            break;
        case LuaValue::Type::Bool:
            out += v.b ? "true" : "false";
            break;
        case LuaValue::Type::Number: {
            char buf[64];
            if (v.n == static_cast<double>(static_cast<long long>(v.n)))
                std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(v.n));
            else
                std::snprintf(buf, sizeof buf, "%g", v.n);
            out += buf;
            break;
        }
        case LuaValue::Type::String:
            out += v.s;
            break;
        default:
            break; // Array/Table: caller handles structure, not reached here
    }
}

void append_record_line(std::string& out, const LuaValue& rec) {
    bool first = true;
    for (auto& kv : rec.tbl) {
        if (!first) out += ' ';
        first = false;
        out += kv.first;
        out += '=';
        append_scalar(out, kv.second);
    }
    out += '\n';
}

} // namespace

namespace lua_env {

void set_shell_callback(ShellCallback cb) { g_shell_cb = std::move(cb); }

lua_State* init() {
    lua_State* L = luaL_newstate();
    luaL_openlibs(L); // full stdlib, no sandboxing -- same trust model as .bashrc
    lua_pushcfunction(L, l_sh);
    lua_setglobal(L, "sh");
    lua_pushcfunction(L, l_setenv);
    lua_setglobal(L, "setenv");
    return L;
}

void close(lua_State* L) { lua_close(L); }

std::string default_config_path() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    std::string base;
    if (xdg && *xdg) {
        base = xdg;
    } else {
        const char* home = std::getenv("HOME");
        if (!home || !*home) return "";
        base = std::string(home) + "/.config";
    }
    return base + "/wisp/init.lua";
}

void load_config(lua_State* L, const std::string& path) {
    if (path.empty()) return;
    struct stat st{};
    if (stat(path.c_str(), &st) != 0) return; // missing -> proceed with empty config
    if (luaL_dofile(L, path.c_str()) != LUA_OK) {
        std::fprintf(stderr, "wisp: error loading %s: %s\n", path.c_str(), lua_tostring(L, -1));
        lua_pop(L, 1); // don't brick the shell over a broken config, matches bash's .bashrc behavior
    }
}

std::string get_prompt(lua_State* L) {
    lua_getglobal(L, "prompt");
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        return "wisp> ";
    }
    if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
        std::fprintf(stderr, "wisp: prompt(): %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
        return "wisp> ";
    }
    const char* s = lua_tostring(L, -1);
    std::string result = s ? s : "wisp> ";
    lua_pop(L, 1);
    return result;
}

CmdKind resolve_command(lua_State* L, const std::string& name) {
    if (is_builtin_name(name)) return CmdKind::Builtin;
    lua_getglobal(L, name.c_str());
    bool is_fn = lua_isfunction(L, -1);
    lua_pop(L, 1);
    return is_fn ? CmdKind::LuaFn : CmdKind::External;
}

LuaValue call_lua_function(lua_State* L, const std::string& name,
                            const std::vector<std::string>& argv,
                            const LuaValue& input, bool has_input) {
    lua_getglobal(L, name.c_str());
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        throw std::runtime_error("wisp: " + name + ": not a function");
    }
    push_luavalue(L, has_input ? input : LuaValue::nil());
    for (auto& a : argv) lua_pushlstring(L, a.data(), a.size());
    int nargs = 1 + static_cast<int>(argv.size());
    if (lua_pcall(L, nargs, 1, 0) != LUA_OK) {
        std::string err = lua_tostring(L, -1) ? lua_tostring(L, -1) : "error";
        lua_pop(L, 1);
        throw std::runtime_error("wisp: " + name + ": " + err);
    }
    LuaValue ret = pull_luavalue(L, -1);
    lua_pop(L, 1);
    return ret;
}

std::string render_value(const LuaValue& v) {
    std::string out;
    switch (v.type) {
        case LuaValue::Type::Nil:
            // A bare top-level nil means "this command returned nothing" --
            // the common print-only-helper case -- so it renders as no
            // output at all, not the noisy literal "nil". A nil *inside* a
            // record (a missing field) still renders via append_scalar.
            break;
        case LuaValue::Type::Array:
            for (auto& elem : v.arr) {
                if (elem.type == LuaValue::Type::Table) {
                    append_record_line(out, elem);
                } else {
                    append_scalar(out, elem);
                    out += '\n';
                }
            }
            break;
        case LuaValue::Type::Table:
            append_record_line(out, v);
            break;
        default:
            append_scalar(out, v);
            out += '\n';
            break;
    }
    return out;
}

} // namespace lua_env
