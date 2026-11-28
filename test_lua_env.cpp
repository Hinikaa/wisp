#include <cassert>
#include <cstdio>

#include "lua_env.h"

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

static void test_resolve_command() {
    lua_State* L = lua_env::init();
    luaL_dostring(L, "function myfunc() return 1 end");

    assert(lua_env::resolve_command(L, "cd") == CmdKind::Builtin);
    assert(lua_env::resolve_command(L, "myfunc") == CmdKind::LuaFn);
    assert(lua_env::resolve_command(L, "/bin/ls") == CmdKind::External);
    assert(lua_env::resolve_command(L, "totally-not-a-command") == CmdKind::External);

    lua_env::close(L);
}

static void test_call_lua_function_input_chaining() {
    lua_State* L = lua_env::init();
    luaL_dostring(L,
        "function first(input, ...) return {10, 20} end\n"
        "function second(input) return {input[1] + 1, input[2] + 1} end\n"
        "function saw_nil(input) if input == nil then return 'yes' else return 'no' end end\n");

    // First stage of a run: has_input=false -> Lua sees input == nil.
    LuaValue r0 = lua_env::call_lua_function(L, "saw_nil", {}, LuaValue::nil(), false);
    assert(r0.type == LuaValue::Type::String && r0.s == "yes");

    LuaValue r1 = lua_env::call_lua_function(L, "first", {}, LuaValue::nil(), false);
    assert(r1.type == LuaValue::Type::Array);
    assert(r1.arr.size() == 2);
    assert(r1.arr[0].type == LuaValue::Type::Number && r1.arr[0].n == 10);

    // Next stage receives the previous stage's return value as input.
    LuaValue r2 = lua_env::call_lua_function(L, "second", {}, r1, true);
    assert(r2.arr[0].n == 11);
    assert(r2.arr[1].n == 21);

    lua_env::close(L);
}

static void test_list_of_records_roundtrip() {
    lua_State* L = lua_env::init();
    luaL_dostring(L, "function rows(input) return { {name='a', size=1}, {name='b', size=2} } end");

    LuaValue r = lua_env::call_lua_function(L, "rows", {}, LuaValue::nil(), false);
    assert(r.type == LuaValue::Type::Array);
    assert(r.arr.size() == 2);
    assert(r.arr[0].type == LuaValue::Type::Table);

    bool found_name = false, found_size = false;
    for (auto& kv : r.arr[0].tbl) {
        if (kv.first == "name") { assert(kv.second.s == "a"); found_name = true; }
        if (kv.first == "size") { assert(kv.second.n == 1); found_size = true; }
    }
    assert(found_name && found_size);

    std::string rendered = lua_env::render_value(r);
    assert(rendered.find("name=a") != std::string::npos);
    assert(rendered.find("name=b") != std::string::npos);

    lua_env::close(L);
}

static void test_from_lines() {
    LuaValue v = LuaValue::from_lines({"one", "two", "three"});
    assert(v.type == LuaValue::Type::Array);
    assert(v.arr.size() == 3);
    assert(v.arr[1].s == "two");
}

static void test_pretty_table_and_prelude() {
    lua_State* L = lua_env::init();
    luaL_dostring(L,
        "rows = { {name='short.lua', size=42}, {name='longer_name.cpp', size=1200} }\n");

    LuaValue sorted = lua_env::call_lua_function(L, "sort_by", {"size", "desc"},
                                                 [&]() {
                                                     lua_getglobal(L, "rows");
                                                     LuaValue v = lua_env::pull_value(L, -1);
                                                     lua_pop(L, 1);
                                                     return v;
                                                 }(),
                                                 true);
    assert(sorted.type == LuaValue::Type::Array && sorted.arr.size() == 2);

    std::string pretty = lua_env::render_value(sorted, true);
    assert(pretty.find("name") != std::string::npos);
    assert(pretty.find("size") != std::string::npos);
    assert(pretty.find("\xe2\x94\x80") != std::string::npos);
    assert(pretty.find("longer_name.cpp") < pretty.find("short.lua"));

    LuaValue filtered = lua_env::call_lua_function(L, "where", {"size", ">", "100"}, sorted, true);
    assert(filtered.type == LuaValue::Type::Array && filtered.arr.size() == 1);

    LuaValue cnt = lua_env::call_lua_function(L, "count", {}, filtered, true);
    assert(cnt.type == LuaValue::Type::Number && cnt.n == 1);

    LuaValue js = lua_env::call_lua_function(L, "to_json", {}, filtered, true);
    assert(js.type == LuaValue::Type::String);
    assert(js.s == "[{\"name\":\"longer_name.cpp\",\"size\":1200}]");

    lua_env::set_status(L, 42);
    luaL_dostring(L, "got_status = wisp.status");
    lua_getglobal(L, "got_status");
    assert(lua_tointeger(L, -1) == 42);
    lua_pop(L, 1);

    lua_env::close(L);
}

int main() {
    test_resolve_command();
    test_call_lua_function_input_chaining();
    test_list_of_records_roundtrip();
    test_from_lines();
    test_pretty_table_and_prelude();
    std::puts("test_lua_env: all tests passed");
    return 0;
}
