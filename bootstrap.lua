#!/usr/bin/env lua
-- bootstrap.lua -- build wisp without make
-- usage: lua bootstrap.lua [build|test|clean|help]

local CXX = os.getenv("CXX") or "g++"
local CC = os.getenv("CC") or "cc"
local FLAGS = "-std=c++17 -O2 -Wall -Wextra"

local SRCS = {"main", "lexer", "parser", "executor", "lua_env"}
local HEADERS = "builtins.h executor.h lexer.h linenoise.h lua_env.h parser.h"
local TESTS = {
    test_lexer = "lexer.o",
    test_parser = "parser.o lexer.o",
    test_lua_env = "lua_env.o",
    test_executor = "executor.o lua_env.o parser.o lexer.o",
}

local function sh(cmd) return os.execute(cmd) end

local function run(cmd)
    io.write("  > " .. cmd .. "\n")
    io.flush()
    return sh(cmd)
end

-- Rebuild when the target is missing or any input is newer. `test -nt` does
-- the timestamp comparison the shell already knows how to do.
local function stale(target, inputs)
    if not sh("test -f " .. target) then return true end
    return sh("test -n \"$(find " .. inputs .. " -newer " .. target .. " 2>/dev/null)\"")
end

local function lua_flags()
    for _, pkg in ipairs({"lua5.4", "lua-5.4", "lua54", "lua"}) do
        local h = io.popen("pkg-config --cflags --libs " .. pkg .. " 2>/dev/null")
        local out = h and h:read("*a") or ""
        if h then h:close() end
        if out:match("%S") then return (out:gsub("%s+", " ")) end
    end
    io.stderr:write("error: lua 5.4 not found via pkg-config\n")
    os.exit(1)
end

local function build(lua)
    local objs = {}
    for _, src in ipairs(SRCS) do
        local obj = src .. ".o"
        objs[#objs + 1] = obj
        if stale(obj, src .. ".cpp " .. HEADERS) then
            if not run(("%s %s %s -c %s.cpp -o %s"):format(CXX, FLAGS, lua, src, obj)) then return false end
        end
    end
    if stale("linenoise.o", "linenoise.c linenoise.h") then
        if not run(("%s -O2 -Wall -c linenoise.c -o linenoise.o"):format(CC)) then return false end
    end
    objs[#objs + 1] = "linenoise.o"

    local all = table.concat(objs, " ")
    if stale("wisp", all) then
        if not run(("%s %s -o wisp %s %s -lpthread"):format(CXX, FLAGS, all, lua)) then return false end
    end
    print("done: ./wisp")
    return true
end

local function test(lua)
    if not build(lua) then return false end
    local passed, failed = 0, 0
    for bin, objs in pairs(TESTS) do
        local ok = run(("%s %s %s -o %s %s.cpp %s -lpthread"):format(CXX, FLAGS, lua, bin, bin, objs))
                   and sh("./" .. bin)
        if ok then passed = passed + 1 else failed = failed + 1 end
    end
    for _, suite in ipairs({"sh test_shell.sh", "python3 test_interactive.py"}) do
        if sh(suite) then passed = passed + 1 else failed = failed + 1 end
    end
    print(("%d/%d test suites passed"):format(passed, passed + failed))
    return failed == 0
end

local function clean()
    run("rm -f *.o *.d wisp test_lexer test_parser test_lua_env test_executor")
    return true
end

local function help()
    print("usage: lua bootstrap.lua build|test|clean|help   (env: CXX, CC)")
    return true
end

local cmd = (arg or {})[1] or "help"
local needs_lua = {build = build, test = test}
local plain = {clean = clean, help = help, ["--help"] = help, ["-h"] = help}

if needs_lua[cmd] then
    os.exit(needs_lua[cmd](lua_flags()) and 0 or 1)
elseif plain[cmd] then
    os.exit(plain[cmd]() and 0 or 1)
end

io.stderr:write("bootstrap.lua: unknown command '" .. cmd .. "'; try 'help'\n")
os.exit(1)
