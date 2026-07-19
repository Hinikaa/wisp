#pragma once

#include <array>
#include <string>

// Fixed builtin name list, shared by executor.cpp (implements them, runs them
// unforked) and lua_env.cpp (needs to classify a bare word as Builtin before
// checking for a same-named Lua function or $PATH executable).
inline bool is_builtin_name(const std::string& name) {
    static constexpr std::array<const char*, 14> kBuiltins = {
        "cd", "exit", "export", "command", "jobs", "fg", "bg", "kill",
        "source", "echo", "disown", "wait", "pwd", "type",
    };
    for (const char* b : kBuiltins)
        if (name == b) return true;
    return false;
}
