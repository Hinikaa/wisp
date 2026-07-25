#!/usr/bin/env lua
-- bootstrap.lua -- build script for wisp
-- usage: lua bootstrap.lua [build|test|clean|help]

local function run(cmd)
    io.write("  > " .. cmd .. "\n")
    io.flush()
    return os.execute(cmd)
end

local function popen_read(cmd)
    local h = io.popen(cmd)
    if not h then return nil end
    local s = h:read("*a")
    h:close()
    return s and s:gsub("%s+$", "")
end

local function find_lua()
    for _, pkg in ipairs({"lua5.4", "lua-5.4", "lua54", "lua"}) do
        local ver = popen_read("pkg-config --modversion " .. pkg .. " 2>/dev/null")
        if ver then
            local cf = popen_read("pkg-config --cflags " .. pkg .. " 2>/dev/null")
            local li = popen_read("pkg-config --libs " .. pkg .. " 2>/dev/null")
            if cf and li then
                return {pkg = pkg, version = ver, cflags = cf, libs = li}
            end
        end
    end
    return nil
end

local CXX = os.getenv("CXX") or "g++"
local CC = os.getenv("CC") or "cc"
local FLAGS = "-std=c++17 -O2 -Wall -Wextra"

local function newer_than(target, source)
    local ta = io.popen("stat -c %Y " .. target .. " 2>/dev/null")
    if not ta then return true end
    local raw = ta:read("*a")
    ta:close()
    local ts = tonumber(raw and raw:match("^%s*(.-)%s*$") or "")
    if not ts then return true end
    local sa = io.popen("stat -c %Y " .. source .. " 2>/dev/null")
    if not sa then return true end
    raw = sa:read("*a")
    sa:close()
    local ss = tonumber(raw and raw:match("^%s*(.-)%s*$") or "")
    if not ss then return true end
    return ss > ts
end

local function build(lua_info)
    print("building wisp (" .. lua_info.pkg .. " " .. lua_info.version .. ")")

    local steps = {
        {out = "main.o",       cmd = ("%s %s %s -c main.cpp -o main.o"):format(CXX, FLAGS, lua_info.cflags),
         deps = {"main.cpp", "executor.h", "lua_env.h", "linenoise.h"}},
        {out = "lexer.o",      cmd = ("%s %s -c lexer.cpp -o lexer.o"):format(CXX, FLAGS),
         deps = {"lexer.cpp", "lexer.h"}},
        {out = "parser.o",     cmd = ("%s %s -c parser.cpp -o parser.o"):format(CXX, FLAGS),
         deps = {"parser.cpp", "parser.h", "lexer.h"}},
        {out = "executor.o",   cmd = ("%s %s %s -c executor.cpp -o executor.o"):format(CXX, FLAGS, lua_info.cflags),
         deps = {"executor.cpp", "executor.h", "lua_env.h", "parser.h", "builtins.h", "pkg_manager.h"}},
        {out = "lua_env.o",    cmd = ("%s %s %s -c lua_env.cpp -o lua_env.o"):format(CXX, FLAGS, lua_info.cflags),
         deps = {"lua_env.cpp", "lua_env.h", "builtins.h"}},
        {out = "pkg_manager.o", cmd = ("%s %s -c pkg_manager.cpp -o pkg_manager.o"):format(CXX, FLAGS),
         deps = {"pkg_manager.cpp", "pkg_manager.h"}},
        {out = "linenoise.o",  cmd = ("%s -O2 -Wall -c linenoise.c -o linenoise.o"):format(CC),
         deps = {"linenoise.c", "linenoise.h"}},
        {out = "wisp",         cmd = ("%s %s -o wisp main.o lexer.o parser.o executor.o lua_env.o linenoise.o pkg_manager.o %s -lpthread"):format(CXX, FLAGS, lua_info.libs),
         deps = {"main.o", "lexer.o", "parser.o", "executor.o", "lua_env.o", "linenoise.o", "pkg_manager.o"}},
    }

    for _, step in ipairs(steps) do
        local need = false
        local chk = io.popen("test -f " .. step.out .. " 2>/dev/null && echo y")
        local exists = chk:read("*a")
        chk:close()
        if not exists or not exists:match("y") then
            need = true
        else
            for _, dep in ipairs(step.deps) do
                if newer_than(step.out, dep) then
                    need = true
                    break
                end
            end
        end
        if need then
            if not run(step.cmd) then return false end
        else
            io.write("  ~ " .. step.out .. " (up to date)\n")
        end
    end

    print("done: ./wisp")
    return true
end

local function test(lua_info)
    local f = io.open("wisp")
    if f then
        f:close()
    else
        if not build(lua_info) then return false end
    end

    local cases = {
        {bin = "test_lexer",    link = " lexer.o",                   lua = false},
        {bin = "test_parser",   link = " parser.o lexer.o",          lua = false},
        {bin = "test_lua_env",  link = " lua_env.o " .. lua_info.libs, lua = true},
        {bin = "test_executor", link = " executor.o lua_env.o parser.o lexer.o pkg_manager.o " .. lua_info.libs .. " -lpthread", lua = true},
    }

    local passed, failed = 0, 0
    for _, t in ipairs(cases) do
        local lua_flag = t.lua and (lua_info.cflags .. " " .. lua_info.libs) or ""
        local cmd = ("%s %s %s -o %s %s.cpp%s"):format(CXX, FLAGS, lua_flag, t.bin, t.bin, t.link)
        if not run(cmd) then
            failed = failed + 1
        else
            io.flush()
            local ok = os.execute("./" .. t.bin .. " 2>&1")
            if ok then
                passed = passed + 1
            else
                failed = failed + 1
            end
        end
    end

    print(("%d/%d tests passed"):format(passed, passed + failed))
    return failed == 0
end

local function clean()
    print("cleaning")
    run("rm -f *.o wisp test_lexer test_parser test_lua_env test_executor")
    return true
end

local function help()
    print([[usage: lua bootstrap.lua <command>

commands:
  build   compile wisp
  test    build and run all tests
  clean   remove build artifacts
  help    show this message

environment:
  CXX     c++ compiler (default: g++)
  CC      c compiler  (default: cc)

features:
  builtins: cd, exit, export, command, jobs, fg, bg, kill,
            source, echo, disown, wait, pwd, type, pkg
  package management: pkg install/remove/list/search/info/update
  structured pipelines: lua tables between native stages
  job control: fg/bg/kill/disown/wait]])
    return true
end

local cmds = {build = build, test = test, clean = clean, help = help}
local arg = arg or {}
local cmd = arg[1] or "help"

if cmd == "help" or cmd == "--help" or cmd == "-h" then
    help()
    os.exit(0)
end

if not cmds[cmd] then
    io.stderr:write("bootstrap.lua: unknown command '" .. cmd .. "'\n")
    io.stderr:write("run 'lua bootstrap.lua help' for usage\n")
    os.exit(1)
end

if cmd ~= "clean" and cmd ~= "help" then
    local lua_info = find_lua()
    if not lua_info then
        io.stderr:write("error: lua not found via pkg-config\n")
        os.exit(1)
    end
    if not cmds[cmd](lua_info) then os.exit(1) end
else
    cmds[cmd]()
end