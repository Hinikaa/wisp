#include "lua_env.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <stdexcept>
#include <sys/stat.h>
#include <unordered_map>

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
lua_env::CaptureCallback g_capture_cb;

lua_State* g_interrupt_L = nullptr;

void interrupt_hook(lua_State* L, lua_Debug*) {
    lua_sethook(L, nullptr, 0, 0);
    luaL_error(L, "interrupted");
}

void interrupt_action(int sig) {
    std::signal(sig, SIG_DFL); // a second Ctrl-C during unwind kills us, same as lua.c
    if (g_interrupt_L)
        lua_sethook(g_interrupt_L, interrupt_hook,
                    LUA_MASKCALL | LUA_MASKRET | LUA_MASKLINE | LUA_MASKCOUNT, 1);
}

int l_sh(lua_State* L) {
    const char* cmdline = luaL_checkstring(L, 1);
    int rc = g_shell_cb ? g_shell_cb(cmdline) : 127;
    lua_pushinteger(L, rc);
    return 1;
}

int l_capture(lua_State* L) {
    const char* cmdline = luaL_checkstring(L, 1);
    if (!g_capture_cb) {
        lua_pushliteral(L, "");
        lua_pushinteger(L, 127);
        return 2;
    }
    auto [out, rc] = g_capture_cb(cmdline);
    lua_pushlstring(L, out.data(), out.size());
    lua_pushinteger(L, rc);
    return 2;
}

int l_setenv(lua_State* L) {
    const char* k = luaL_checkstring(L, 1);
    const char* v = luaL_checkstring(L, 2);
    setenv(k, v, 1);
    return 0;
}

std::string expand_tilde_path(const std::string& p) {
    if (p == "~" || (p.size() >= 2 && p[0] == '~' && p[1] == '/')) {
        if (const char* home = std::getenv("HOME"); home && *home)
            return std::string(home) + p.substr(1);
    }
    return p;
}

// `files [-a] [dir]` -- structured directory listing returning a list of
// `{ name, type, size, mtime }` records. Works both as a shell command
// (pipeline input in arg 1, flags/dir in args 2..) and directly from Lua.
int l_files(lua_State* L) {
    int n = lua_gettop(L);
    int start = (n >= 1 && lua_isstring(L, 1)) ? 1 : 2;
    bool show_all = false;
    std::string dir = ".";
    for (int i = start; i <= n; ++i) {
        if (!lua_isstring(L, i)) continue;
        std::string a = lua_tostring(L, i);
        if (a == "-a" || a == "--all") show_all = true;
        else dir = expand_tilde_path(a);
    }

    DIR* d = opendir(dir.c_str());
    if (!d) return luaL_error(L, "%s: %s", dir.c_str(), std::strerror(errno));

    struct Entry {
        std::string name;
        const char* type;
        lua_Integer size;
        std::string mtime;
    };
    std::vector<Entry> entries;
    while (struct dirent* ent = readdir(d)) {
        std::string name = ent->d_name;
        if (name == "." || name == "..") continue;
        if (!show_all && !name.empty() && name[0] == '.') continue;

        std::string full = (!dir.empty() && dir.back() == '/') ? (dir + name) : (dir + "/" + name);
        struct stat st{};
        const char* kind = "other";
        lua_Integer sz = 0;
        char tbuf[32] = "";
        if (lstat(full.c_str(), &st) == 0) {
            if (S_ISREG(st.st_mode)) kind = "file";
            else if (S_ISDIR(st.st_mode)) kind = "dir";
            else if (S_ISLNK(st.st_mode)) kind = "link";
            sz = static_cast<lua_Integer>(st.st_size);
            struct tm tm_buf{};
            if (localtime_r(&st.st_mtime, &tm_buf))
                std::strftime(tbuf, sizeof tbuf, "%Y-%m-%d %H:%M", &tm_buf);
        }
        entries.push_back({std::move(name), kind, sz, tbuf});
    }
    closedir(d);

    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) { return a.name < b.name; });

    lua_createtable(L, static_cast<int>(entries.size()), 0);
    for (size_t i = 0; i < entries.size(); ++i) {
        lua_createtable(L, 0, 4);
        lua_pushlstring(L, entries[i].name.data(), entries[i].name.size());
        lua_setfield(L, -2, "name");
        lua_pushstring(L, entries[i].type);
        lua_setfield(L, -2, "type");
        lua_pushinteger(L, entries[i].size);
        lua_setfield(L, -2, "size");
        lua_pushlstring(L, entries[i].mtime.data(), entries[i].mtime.size());
        lua_setfield(L, -2, "mtime");
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    return 1;
}

// Built-in structured pipeline helpers loaded before ~/.config/wisp/init.lua,
// so users can use them out of the box or override any of them in their config.
const char kPrelude[] = R"lua(
function where(rows, a, op, val)
  if type(rows) ~= "table" then return {} end
  local out = {}
  if op == nil then
    local pat = tostring(a or "")
    for _, row in ipairs(rows) do
      if type(row) == "table" then
        for _, v in pairs(row) do
          if tostring(v):find(pat) then out[#out + 1] = row; break end
        end
      elseif tostring(row):find(pat) then
        out[#out + 1] = row
      end
    end
    return out
  end
  local num_rhs = tonumber(val)
  for _, row in ipairs(rows) do
    local lhs = type(row) == "table" and row[a] or nil
    local ok = false
    if op == "~=" then
      ok = lhs ~= nil and tostring(lhs):find(tostring(val or "")) ~= nil
    elseif lhs ~= nil then
      local num_lhs = tonumber(lhs)
      local l, r
      if num_lhs and num_rhs then l, r = num_lhs, num_rhs
      else l, r = tostring(lhs), tostring(val or "") end
      if op == "==" or op == "=" then ok = l == r
      elseif op == "!=" then ok = l ~= r
      elseif op == ">" then ok = l > r
      elseif op == ">=" then ok = l >= r
      elseif op == "<" then ok = l < r
      elseif op == "<=" then ok = l <= r
      else error("where: unknown operator '" .. tostring(op) .. "'") end
    end
    if ok then out[#out + 1] = row end
  end
  return out
end

function sort_by(rows, field, order)
  if type(rows) ~= "table" then return {} end
  local out = {}
  for i = 1, #rows do out[i] = rows[i] end
  local desc = (order == "desc" or order == "-r")
  table.sort(out, function(a, b)
    local va = type(a) == "table" and a[field] or a
    local vb = type(b) == "table" and b[field] or b
    if va == vb then return false end
    if va == nil then return false end
    if vb == nil then return true end
    local lt
    if type(va) == "number" and type(vb) == "number" then lt = va < vb
    else lt = tostring(va) < tostring(vb) end
    if desc then return not lt else return lt end
  end)
  return out
end

function cols(rows, ...)
  if type(rows) ~= "table" then return {} end
  local fields = { ... }
  local out = {}
  for _, row in ipairs(rows) do
    if type(row) == "table" then
      local rec = {}
      for _, f in ipairs(fields) do rec[f] = row[f] end
      out[#out + 1] = rec
    end
  end
  return out
end

function take(rows, n)
  if type(rows) ~= "table" then return {} end
  n = tonumber(n) or 10
  local out = {}
  for i = 1, math.min(#rows, math.max(0, math.floor(n))) do out[i] = rows[i] end
  return out
end

function skip(rows, n)
  if type(rows) ~= "table" then return {} end
  n = math.max(0, math.floor(tonumber(n) or 1))
  local out = {}
  for i = n + 1, #rows do out[#out + 1] = rows[i] end
  return out
end

function count(rows)
  if type(rows) ~= "table" then return rows == nil and 0 or 1 end
  local n = #rows
  if n > 0 then return n end
  for _ in pairs(rows) do n = n + 1 end
  return n
end

function lines(input)
  if type(input) == "table" then return input end
  if input == nil then return {} end
  local s = tostring(input)
  local out = {}
  for line in (s .. "\n"):gmatch("([^\n]*)\n") do out[#out + 1] = line end
  if #out > 0 and out[#out] == "" and s:sub(-1) == "\n" then out[#out] = nil end
  return out
end

local function json_esc(s)
  return '"' .. s:gsub('[%z\1-\31\\"]', function(c)
    if c == '"' then return '\\"'
    elseif c == '\\' then return '\\\\'
    elseif c == '\n' then return '\\n'
    elseif c == '\r' then return '\\r'
    elseif c == '\t' then return '\\t'
    elseif c == '\b' then return '\\b'
    elseif c == '\f' then return '\\f'
    else return string.format('\\u%04x', c:byte()) end
  end) .. '"'
end

local function json_encode(v)
  local t = type(v)
  if v == nil then return "null"
  elseif t == "boolean" then return v and "true" or "false"
  elseif t == "number" then
    if v ~= v or v == math.huge or v == -math.huge then return "null" end
    if math.type(v) == "float" and math.floor(v) == v and math.abs(v) < 9007199254740992 then
      return string.format("%.0f", v)
    end
    return tostring(v)
  elseif t == "string" then return json_esc(v)
  elseif t == "table" then
    local n, cnt = #v, 0
    local is_arr = true
    for k in pairs(v) do
      cnt = cnt + 1
      if type(k) ~= "number" or k < 1 or k > n or math.floor(k) ~= k then is_arr = false end
    end
    if is_arr and cnt == n and n > 0 then
      local parts = {}
      for i = 1, n do parts[i] = json_encode(v[i]) end
      return "[" .. table.concat(parts, ",") .. "]"
    end
    local keys = {}
    for k in pairs(v) do if type(k) == "string" then keys[#keys + 1] = k end end
    table.sort(keys)
    local parts = {}
    for i, k in ipairs(keys) do parts[i] = json_esc(k) .. ":" .. json_encode(v[k]) end
    return "{" .. table.concat(parts, ",") .. "}"
  end
  return "null"
end

function to_json(input)
  return json_encode(input)
end
)lua";

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
        lua_pop(L, 1);
        if (!lua_isinteger(L, -1)) {
            lua_pop(L, 1);
            return false;
        }
        ++count;
        if (count > len) {
            lua_pop(L, 1);
            return false;
        }
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
                v.arr.reserve(static_cast<size_t>(len));
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
            if (std::isfinite(v.n) && v.n >= -9007199254740992.0 && v.n <= 9007199254740992.0 &&
                std::trunc(v.n) == v.n)
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

int key_rank(const std::string& k) {
    static const char* const kPrimary[] = {"name", "id", "pid", "type", "size", "mtime"};
    for (int i = 0; i < 6; ++i)
        if (k == kPrimary[i]) return i;
    return 100;
}

bool key_less(const std::string& a, const std::string& b) {
    int ra = key_rank(a), rb = key_rank(b);
    return ra != rb ? ra < rb : a < b;
}

void append_record_line(std::string& out, const LuaValue& rec) {
    std::vector<const std::pair<std::string, LuaValue>*> items;
    items.reserve(rec.tbl.size());
    for (auto& kv : rec.tbl) items.push_back(&kv);
    std::sort(items.begin(), items.end(),
              [](const auto* a, const auto* b) { return key_less(a->first, b->first); });
    bool first = true;
    for (auto* kv : items) {
        if (!first) out += ' ';
        first = false;
        out += kv->first;
        out += '=';
        append_scalar(out, kv->second);
    }
    out += '\n';
}

std::string scalar_str(const LuaValue& v) {
    std::string s;
    append_scalar(s, v);
    return s;
}

// Column-aligned table rendering for interactive TTY output. Keeps external
// pipe boundaries on compact `key=value` lines while giving terminal users
// readable columns with right-aligned numbers.
std::string render_pretty_table(const std::vector<LuaValue>& rows) {
    std::vector<std::string> cols;
    std::unordered_map<std::string, size_t> col_idx;
    for (auto& row : rows) {
        for (auto& kv : row.tbl) {
            if (col_idx.find(kv.first) == col_idx.end()) {
                col_idx[kv.first] = cols.size();
                cols.push_back(kv.first);
            }
        }
    }
    if (cols.empty()) return "";
    std::sort(cols.begin(), cols.end(), key_less);
    col_idx.clear();
    for (size_t i = 0; i < cols.size(); ++i) col_idx[cols[i]] = i;

    std::vector<size_t> widths(cols.size());
    std::vector<bool> numeric(cols.size(), true);
    for (size_t c = 0; c < cols.size(); ++c) widths[c] = cols[c].size();

    std::vector<std::vector<std::string>> cells(rows.size(), std::vector<std::string>(cols.size()));
    for (size_t r = 0; r < rows.size(); ++r) {
        for (auto& kv : rows[r].tbl) {
            size_t c = col_idx[kv.first];
            if (kv.second.type != LuaValue::Type::Number) numeric[c] = false;
            cells[r][c] = scalar_str(kv.second);
            if (cells[r][c].size() > widths[c]) widths[c] = cells[r][c].size();
        }
    }

    std::string out;
    auto append_pad = [&](const std::string& s, size_t w, bool right) {
        size_t pad = w > s.size() ? w - s.size() : 0;
        if (right) out.append(pad, ' ');
        out += s;
        if (!right) out.append(pad, ' ');
    };

    for (size_t c = 0; c < cols.size(); ++c) {
        if (c > 0) out += "  ";
        append_pad(cols[c], widths[c], numeric[c]);
    }
    out += '\n';
    for (size_t c = 0; c < cols.size(); ++c) {
        if (c > 0) out += "  ";
        for (size_t k = 0; k < widths[c]; ++k) out += "\xe2\x94\x80";
    }
    out += '\n';
    for (size_t r = 0; r < rows.size(); ++r) {
        for (size_t c = 0; c < cols.size(); ++c) {
            if (c > 0) out += "  ";
            append_pad(cells[r][c], widths[c], numeric[c]);
        }
        out += '\n';
    }
    return out;
}

std::string render_pretty_record(const LuaValue& rec) {
    std::vector<const std::pair<std::string, LuaValue>*> items;
    items.reserve(rec.tbl.size());
    for (auto& kv : rec.tbl) items.push_back(&kv);
    std::sort(items.begin(), items.end(),
              [](const auto* a, const auto* b) { return key_less(a->first, b->first); });
    size_t max_k = 0;
    for (auto* kv : items)
        if (kv->first.size() > max_k) max_k = kv->first.size();
    std::string out;
    for (auto* kv : items) {
        out += kv->first;
        out.append(max_k - kv->first.size() + 2, ' ');
        append_scalar(out, kv->second);
        out += '\n';
    }
    return out;
}

} // namespace

namespace lua_env {

InterruptGuard::InterruptGuard(lua_State* L) {
    g_interrupt_L = L;
    struct sigaction sa {};
    sa.sa_handler = interrupt_action;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, &old_);
}

InterruptGuard::~InterruptGuard() {
    sigaction(SIGINT, &old_, nullptr);
    g_interrupt_L = nullptr;
}

void set_shell_callback(ShellCallback cb) { g_shell_cb = std::move(cb); }
void set_capture_callback(CaptureCallback cb) { g_capture_cb = std::move(cb); }

void set_status(lua_State* L, int status) {
    lua_getglobal(L, "wisp");
    if (lua_istable(L, -1)) {
        lua_pushinteger(L, status);
        lua_setfield(L, -2, "status");
    }
    lua_pop(L, 1);
}

lua_State* init() {
    lua_State* L = luaL_newstate();
    luaL_openlibs(L); // full stdlib, no sandboxing -- same trust model as .bashrc
    lua_pushcfunction(L, l_sh);
    lua_setglobal(L, "sh");
    lua_pushcfunction(L, l_capture);
    lua_setglobal(L, "capture");
    lua_pushcfunction(L, l_setenv);
    lua_setglobal(L, "setenv");
    lua_pushcfunction(L, l_files);
    lua_setglobal(L, "files");

    lua_createtable(L, 0, 2);
    lua_pushliteral(L, "3.2.1");
    lua_setfield(L, -2, "version");
    lua_pushinteger(L, 0);
    lua_setfield(L, -2, "status");
    lua_setglobal(L, "wisp");

    if (luaL_dostring(L, kPrelude) != LUA_OK) {
        std::fprintf(stderr, "wisp: internal prelude error: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
    }

    // Where `pkg install` (luarocks --local) actually puts things, plus a
    // plain drop-in directory. cpath matters as much as path: without it no
    // rock with a C component can load at all. Spelled out rather than asked
    // of `luarocks path`, which would cost a fork on every startup.
    if (const char* home = std::getenv("HOME"); home && *home) {
        std::string h(home);
        std::string path_prefix =
            h + "/.luarocks/share/lua/" LUA_VDIR "/?.lua;" +
            h + "/.luarocks/share/lua/" LUA_VDIR "/?/init.lua;" +
            h + "/.wisp/packages/?.lua;" + h + "/.wisp/packages/?/init.lua;";
        std::string cpath_prefix =
            h + "/.luarocks/lib/lua/" LUA_VDIR "/?.so;";
        lua_getglobal(L, "package");
        if (lua_istable(L, -1)) {
            lua_getfield(L, -1, "path");
            if (lua_isstring(L, -1)) {
                lua_pushlstring(L, path_prefix.data(), path_prefix.size());
                lua_insert(L, -2);
                lua_concat(L, 2);
                lua_setfield(L, -2, "path");
            } else {
                lua_pop(L, 1);
            }
            lua_getfield(L, -1, "cpath");
            if (lua_isstring(L, -1)) {
                lua_pushlstring(L, cpath_prefix.data(), cpath_prefix.size());
                lua_insert(L, -2);
                lua_concat(L, 2);
                lua_setfield(L, -2, "cpath");
            } else {
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
    }

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

void push_value(lua_State* L, const LuaValue& v) { push_luavalue(L, v); }
LuaValue pull_value(lua_State* L, int idx) { return pull_luavalue(L, idx); }

void call_lua_function_top(lua_State* L, const std::string& name,
                           const std::vector<std::string>& argv, bool has_input) {
    lua_getglobal(L, name.c_str());
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        if (has_input) lua_pop(L, 1);
        throw std::runtime_error("wisp: " + name + ": not a function");
    }
    if (has_input) lua_insert(L, -2); // slide the function under its input
    else lua_pushnil(L);
    for (auto& a : argv) lua_pushlstring(L, a.data(), a.size());
    if (lua_pcall(L, 1 + static_cast<int>(argv.size()), 1, 0) != LUA_OK) {
        std::string err = lua_tostring(L, -1) ? lua_tostring(L, -1) : "error";
        lua_pop(L, 1);
        throw std::runtime_error("wisp: " + name + ": " + err);
    }
}

LuaValue call_lua_function(lua_State* L, const std::string& name,
                            const std::vector<std::string>& argv,
                            const LuaValue& input, bool has_input) {
    if (has_input) push_luavalue(L, input);
    call_lua_function_top(L, name, argv, has_input);
    LuaValue ret = pull_luavalue(L, -1);
    lua_pop(L, 1);
    return ret;
}

std::string render_value(const LuaValue& v, bool pretty) {
    std::string out;
    switch (v.type) {
        case LuaValue::Type::Nil:
            // A bare top-level nil means "this command returned nothing" --
            // the common print-only-helper case -- so it renders as no
            // output at all, not the noisy literal "nil". A nil *inside* a
            // record (a missing field) still renders via append_scalar.
            break;
        case LuaValue::Type::Array:
            if (pretty && !v.arr.empty()) {
                bool all_tables = true;
                for (auto& elem : v.arr) {
                    if (elem.type != LuaValue::Type::Table || elem.tbl.empty()) {
                        all_tables = false;
                        break;
                    }
                }
                if (all_tables) return render_pretty_table(v.arr);
            }
            out.reserve(v.arr.size() * 16);
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
            if (pretty && !v.tbl.empty()) return render_pretty_record(v);
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
