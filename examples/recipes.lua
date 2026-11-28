-- Practical wisp recipes. Load with `source examples/recipes.lua` or copy
-- individual functions into ~/.config/wisp/init.lua.

-- Top processes by resident memory (KB):
--   wisp> top_mem 10
function top_mem(_, n)
    return take(sort_by(procs(), "mem_kb", "desc"), tonumber(n) or 10)
end

-- Structured git log:
--   wisp> git_log 10 | where author ~= hina
function git_log(_, n)
    local out, rc = capture("git log -n " .. (tonumber(n) or 15) .. " --format='%h\t%an\t%ad\t%s' --date=short")
    if rc ~= 0 or out == "" then return {} end
    local rows = {}
    for _, line in ipairs(lines(out)) do
        local hash, author, date, msg = line:match("([^\t]*)\t([^\t]*)\t([^\t]*)\t(.*)")
        if hash then
            rows[#rows + 1] = { hash = hash, author = author, date = date, message = msg }
        end
    end
    return rows
end

-- Fetch JSON from a URL into Lua tables:
--   wisp> json_get https://api.github.com/users/Hinikaa/repos | sort_by stargazers_count desc | cols name stargazers_count
function json_get(_, url)
    if not url then error("usage: json_get <url>") end
    local out, rc = capture("curl -fsSL '" .. url:gsub("'", "'\\''") .. "'")
    if rc ~= 0 then error("curl failed (" .. rc .. ")") end
    return from_json(out)
end

-- Count files by extension in the current directory:
--   wisp> ext_counts | sort_by count desc
function ext_counts(_, dir)
    local counts = {}
    for _, f in ipairs(files(dir or ".")) do
        if f.type == "file" then
            local ext = f.name:match("%.([^%.]+)$") or "(none)"
            counts[ext] = (counts[ext] or 0) + 1
        end
    end
    local rows = {}
    for ext, c in pairs(counts) do rows[#rows + 1] = { ext = ext, count = c } end
    return rows
end
