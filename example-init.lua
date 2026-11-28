-- Example wisp config. Copy to ~/.config/wisp/init.lua and edit.
--
-- There is no config DSL: this is plain Lua 5.4, and every global function
-- you define here becomes a command you can run at the prompt.

function prompt()
    local branch = capture("git branch --show-current 2>/dev/null")
    local git = branch ~= "" and (" (" .. branch .. ")") or ""
    local err = wisp.status ~= 0 and (" [" .. wisp.status .. "]") or ""
    return "wisp" .. git .. err .. "> "
end

-- Plain command: first arg is pipeline input (nil if none), rest are shell args.
function greet(_, who)
    print("hello, " .. (who or "world"))
end

-- Aliases are just functions calling sh().
function ll()
    sh("ls -la")
end

-- Custom structured pipeline stage: receives the previous stage's Lua table
-- directly in-process and returns a new table for the next stage.
function big_files()
    return where(files(), "size", ">", 1024)
end

-- Out of the box, wisp includes: files, where, sort_by, cols, take, skip,
-- count, lines, and to_json. Try:
--   wisp> files | where type == file | sort_by size desc | take 5 | cols name size
--   wisp> files | where name ~= %.cpp$ | count
--   wisp> big_files | sort_by size desc | to_json
