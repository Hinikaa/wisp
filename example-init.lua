-- Example wisp config. Copy to ~/.config/wisp/init.lua and edit.
--
-- There is no config DSL: this is ordinary Lua, and every global function you
-- define here becomes a command you can type at the prompt.

function prompt()
    return "wisp> "
end

-- A plain command. The first parameter is the pipeline input (nil when this
-- is the first stage); the rest are the shell words you typed after it.
function greet(_, who)
    print("hello, " .. (who or "world"))
end

-- Aliases are just functions -- no separate alias syntax to learn.
function ll()
    sh("ls -la")
end

-- A pipeline source: returns a list of records rather than text.
function nums()
    return { { n = 3, tag = "c" }, { n = 1, tag = "a" }, { n = 2, tag = "b" } }
end

-- A pipeline stage: receives the previous stage's value and returns the next
-- one. No parsing, no re-serializing -- `input` is the actual Lua table.
function sorted(input)
    table.sort(input, function(a, b) return a.n < b.n end)
    return input
end

--   wisp> nums | sorted           -- both stages in-process, no fork
--   wisp> nums | sorted | wc -l   -- rendered to text only at the boundary
