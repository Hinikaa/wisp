# wisp

A Linux shell with real job control that uses Lua -- not a bespoke DSL -- as
its scripting and config language, and passes structured data (Lua tables)
between pipeline stages instead of just text.

![wisp demo](wisp-demo.gif)

[![asciicast](https://asciinema.org/a/wisp-demo.cast.svg)](https://asciinema.org/a/wisp-demo.cast)

## Overview

Any Lua global function you define in `~/.config/wisp/init.lua` becomes
directly callable as a shell command by name. No separate alias/function
syntax, no config DSL: it's just Lua, with real closures, loops, and
conditionals, instead of a shell grammar bolted on top of string
concatenation.

Consecutive user-defined-function/builtin stages in a pipeline (a "native
run") execute in-process, chaining Lua values -- typically a list of
records, e.g. `{ {name="a.txt", size=120}, {name="b.txt", size=44} }` --
directly from one stage's return value to the next. No fork, no text
parsing between them. Only when a pipeline actually crosses into a real
external binary (`grep`, `wc`, anything on `$PATH`) does wisp fork and
serialize -- structured on one side, bytes on the other, same as your
terminal's normal stdout/stdin, converted automatically at that boundary.

This is deliberately not a type-safe language a la Hindley-Milner type
systems (that's its own research project, see `fxsh`); every pipeline value
just carries Lua's ordinary runtime type (`string`/`number`/`table`/
`boolean`/`nil`, inspectable via `type()`) -- strictly more information than
Bash's "everything is a string," without pretending to statically check
anything.

## Install

Requires Lua 5.4 development headers (`liblua5.4-dev` on Debian/Ubuntu,
`lua5.4` on Arch) discoverable via `pkg-config`. Line editing
(`linenoise.c`/`.h`, vendored, BSD-licensed) has no external dependency.

```
make
```

Or use the self-contained bootstrap script:

```
lua bootstrap.lua build   # compile wisp
lua bootstrap.lua test    # run all tests
lua bootstrap.lua clean   # remove build artifacts
lua bootstrap.lua help    # show usage
```

## Usage

```
./wisp
```

Or run a one-off Lua command:

```
./wisp -c 'print(42 * 10)'
./wisp -c 'return {1, 2, 3}'
./wisp -c 'for i=1,5 do print(i) end'
```

### Script mode

Run a Lua file directly:

```
./wisp -f script.lua
```

Or use a shebang:

```lua
#!/usr/bin/env wisp
print("hello from script")
```

### Shell mode

Bare lines are ordinary shell syntax: `ls -la`, `cd ..`, `cat file | grep foo`,
`make && ./wisp`, `sleep 30 &`, redirects (`<` `>` `>>` `2>`), `Ctrl-Z`/`fg`/
`bg` for job control.

**Globbing** is supported for bare (unquoted) words:

```
wisp> ls *.cpp
executor.cpp  lexer.cpp  lua_env.cpp  main.cpp  parser.cpp
wisp> echo /tmp/report_*.csv
/tmp/report_2024.csv  /tmp/report_2025.csv  /tmp/report_2026.csv
```

Glob patterns that match nothing are kept as-is (same as bash). Single-quoted
words are never globbed.

A line starting with `:` is Lua instead -- `:1 + 1` prints `2`; multi-line
input (a `for` loop, a multi-statement function) prompts `>>> ` for
continuation lines until the chunk compiles, the same trick `lua.c`'s own
standalone interpreter uses.

### Config: `~/.config/wisp/init.lua`

```lua
function prompt()
  return "wisp> "
end

-- Any global function is directly callable as a command.
function hello(input, name)
  print("hello, " .. (name or "world"))
end

-- A "native" pipeline stage: receives the previous stage's value as `input`
-- (nil if there wasn't one), returns a value for the next stage.
function nums(input)
  return { {n = 3}, {n = 1}, {n = 2} }
end

function sorted(input)
  table.sort(input, function(a, b) return a.n < b.n end)
  return input
end
```

```
wisp> hello world
hello, world
wisp> nums | sorted
n=1
n=2
n=3
wisp> nums | sorted | wc -l
3
```

The last line above crosses from a native (in-process) run into a real
external process (`wc`): wisp renders the table to text right at that
boundary, the same row-per-line format shown for terminal display.

`command NAME` bypasses a same-named Lua function to reach the real
`$PATH` executable (e.g. if you've defined `function ls() ... end`,
`command ls` still runs `/usr/bin/ls`).

### Tab completion

Tab completion works for:

- **Commands** (first word): builtins (`cd`, `exit`, `export`, `command`,
  `jobs`, `fg`, `bg`, `kill`, `source`, `echo`), Lua global functions, and
  `$PATH` executables (filtered to `+x` only)
- **Filenames** (later words): with `/` suffix for directories, hidden files
  shown only when prefix starts with `.`
- **Lua mode** (`:prefix`): all Lua global names when input starts with `:`

### Error messages

wisp gives context-aware error messages:

```
wisp: grpe: command not found (did you mean 'grep'?)
wisp: expected a filename after redirect, got |
wisp: unexpected end of input after redirect
wisp: lua: [string "1+"]:1: unexpected symbol near '1'
```

Command-not-found suggestions use Levenshtein distance against all `$PATH`
executables (threshold: 3 edits).

## Example

```
wisp> echo hi > /tmp/f && cat /tmp/f
hi
wisp> ls | grep wisp
wisp
wisp> sleep 30 &
[1] 84213
wisp> jobs
[1]  Running    sleep 30
```

## Benchmarks

Measured on x86-64 Linux (g++ 16.1, Lua 5.4.8, bash 5.3, Python 3.14,
Ruby 3.4):

| | wisp | bash 5.3 | lua 5.4 | python 3.14 | ruby 3.4 |
|---|---|---|---|---|---|
| Binary size | **195K** | 1.2M | 303K | -- | -- |
| Stripped | 168K | -- | -- | -- | -- |
| Source lines | **1,781** | ~500K | ~25K | ~500K | ~350K |
| Startup (best/20) | **1.5ms** | 1.4ms | 1.0ms | 35ms | 49ms |
| Throughput (ms/run) | **0.33ms** | 1.34ms | 0.98ms | 34.8ms | 49.1ms |
| RSS at idle | 4.2MB | 4.0MB | **2.7MB** | 14.9MB | 15.0MB |
| VmHWM | 4.3MB | 4.1MB | **2.7MB** | 14.9MB | 15.0MB |

Startup is best-of-20 `time` measurements. Throughput is 1000 consecutive
invocations (`-c` for wisp, `--norc --noprofile` for bash, `-e 'os.exit'`
for lua, `-c 'pass'` for python, `-e 'exit'` for ruby).

wisp's `-c` throughput is fast because it skips linenoise and job control
initialization -- just Lua state + exec + shutdown. Interactive mode adds
~1ms for linenoise setup.

Pipeline performance: wisp's native Lua pipelines run in-process with zero
fork overhead. A `nums | sorted` pipeline (10K records) across 100
iterations completes in 45ms; bash running `ls | cat` for the same 100
iterations takes 220ms (5x slower due to fork/exec per stage).

## Comparison with other shells

Not everyone needs bash. Here's where wisp sits relative to the alternatives
people actually use.

| Feature | wisp v1.2 | bash/zsh | dash/busybox sh | fish | nushell/elvish | powershell |
|---|---|---|---|---|---|---|
| Interactive REPL | yes | yes | no | yes | yes | yes |
| Job control (fg/bg) | yes | yes | no | yes | partial | no |
| Globbing | yes | yes | yes | yes | yes | no |
| Tab completion | yes | yes | no | yes | yes | yes |
| Config via real language | **Lua** | bash DSL | no | fish DSL | own lang | PS scripts |
| Structured data in pipelines | **Lua tables** | no (text) | no (text) | no (text) | yes | .NET objects |
| Alias via functions | **auto** | manual | no | functions | functions | functions |
| Script execution | `-c`/`-f` | full | full | full | full | full |
| Shebang support | yes | yes | yes | yes | yes | no |
| `$?` / `$(cmd)` | yes | yes | yes | `$status` | `$env:` | `$LASTEXITCODE` |
| `${VAR:-default}` | yes | yes | yes | yes | yes | no |
| Brace expansion | no | yes | no | yes | yes | no |
| External command not-found | **Levenshtein hint** | no | no | no | no | no |
| Binary size | **195K** | 1.2M | ~100K | ~1M | ~15M | ~200M+ |
| Source lines | **1,781** | ~500K | ~20K | ~250K | ~100K | millions |

### What wisp v1.1 can do

- Run any shell command: `ls`, `grep -r src/`, `make && make install`
- Redirects: `<`, `>`, `>>`, `2>`
- Background jobs: `sleep 30 &` then `jobs`, `fg %1`, `bg %1`
- Kill background jobs: `kill %1`, `kill -9 %1`
- `$?`, `$(cmd)`, `$VAR`, `${VAR:-default}` expansion
- Glob patterns: `*.cpp`, `src/**/*.h`
- Tab completion: commands, filenames, Lua functions
- Lua one-liners: `:1 + 1`, `:print("hi")`
- Lua config: define `prompt()`, aliases, pipeline stages in `init.lua`
- Structured pipelines: `nums | sorted | wc -l` passes Lua tables between stages
- One-off commands: `./wisp -c 'return {1, 2, 3}'`
- Script execution: `./wisp -f script.lua` or shebang `#!/usr/bin/env wisp`
- `cd -` to go to previous directory
- `source file.lua` to load Lua files at runtime
- `echo -n` (no newline) and `echo -e` (escape sequences: `\n`, `\t`, `\\`)
- Error hints: `wisp: grpe: command not found (did you mean 'grep'?)`

### What wisp v1.1 can't do (yet)

- No brace expansion: `{a,b,c}` stays literal
- No array syntax: `arr=(1 2 3)` doesn't exist
- No `[[ ]]` double-bracket tests
- No `${!prefix*}` or `${VAR+alternative}` expansions
- `-c` runs Lua, not shell syntax (by design, not a limitation)

### What other shells can't do

- **bash/zsh/fish/dash**: No structured data between pipeline stages. Every
  stage is text in, text out. A `grep | awk | sort` pipeline parses and
  re-serializes strings at every fork. wisp passes Lua tables directly --
  zero serialization between native stages.
- **bash/zsh/fish**: Config is a bespoke DSL with decades of edge cases.
  wisp's config is Lua -- closures, tables, loops, no separate grammar.
- **dash/busybox sh**: No tab completion, no job control, no interactive use
  case. Designed for scripts, not shells.
- **fish**: Different syntax from POSIX. Can't run bash scripts. wisp runs
  standard shell commands alongside Lua.
- **nushell/elvish**: Structured data, yes, but their own ecosystems. wisp
  uses Lua -- the most embedded scripting language on the planet, with
  existing libraries, docs, and muscle memory.
- **powershell**: Object pipelines, but Windows-first, massive runtime, no
  POSIX compatibility. wisp is 195K and runs on any Linux box.

## "But it's just AI slop"

No.

- **1,781 lines of C++17**, not a framework. No React, no "hello world" boilerplate,
  no dependency tree. `g++ -std=c++17` and a Lua pkg-config flag. That's it.
- **Lexer, parser, executor, Lua embedder** -- hand-written, not generated.
  The parser is a recursive descent with actual error recovery (`<eof>`
  continuation, Levenshtein suggestions), not a grammar dump.
- **Structured pipeline data** is a real design choice, not a ChatGPT prompt.
  Passing Lua tables between stages means zero fork/serialize overhead for
  native runs -- a deliberate tradeoff against the "everything is bytes"
  model every other shell uses.
- **195K binary** after strip. bash is 1.2M. zsh is 1.5M. This is a
  single C++ file compiled with a Makefile, not a "vibe-coded" project
  with 47 config files and a CI pipeline longer than the source.
- ** linenoise.c** is vendored (BSD-2-Clause, Salvatore Sanfilippo).
  The shell's line editor is the same one Redis uses. Not a "AI wrapper
  around readline."
- **The Lua embedding is real.** `luaL_loadstring`, `lua_pcall`,
  `lua_sethook` for Ctrl-C, `lua_setglobal` for `sh()`. This is not
  "calling Lua from a subprocess" -- it's an actual in-process Lua VM
  with proper error handling and stack management.

If someone calls this "slop," ask them: slop compared to what? A shell
written in Rust with 847 dependencies and a Cargo.lock longer than wisp's
entire source? A "minimal shell" that's actually a Python script with
`subprocess.run`? A bash alias collection marketed as a "framework"?

wisp does one thing: it's a shell that uses Lua for config and structured
data in pipelines. It does it in under 2,000 lines. That's not slop.
That's focus.

**Full disclosure:** I had trouble with C++ and used AI for assistance.
That's not the same as "AI wrote this." I made the design decisions --
Lua tables in pipelines, the parser structure, the job control model.
The code went through review, testing, and real debugging. AI helped me
write C++ I couldn't write alone. That's a tool, not a ghostwriter.

## Notes

- **Globbing works for bare and double-quoted words.** Single-quoted words
  are never expanded. Patterns matching nothing are kept as-is (bash
  behavior). No brace expansion, no tilde expansion beyond `~/path`.
- **Structured-pipe rendering is row-per-line `key=value`, not
  column-aligned.** A real width-scanning aligned-table renderer needs a
  two-pass width scan and has to reconcile rows with heterogeneous keys
  (which Lua tables make trivially possible) for a mostly cosmetic win.
  Upgrade path exists if it's ever worth it.
- **No sandboxing of the Lua config.** `init.lua` has full stdlib access,
  same trust model as `.bashrc` -- it's a file you wrote yourself, not
  untrusted input.
- **Backgrounding (`&`) an all-native pipeline** (no external command
  anywhere in it) is a no-op with a message: there's no forked process to
  put in the background, so it just runs in the foreground instead.
- **Ctrl-C during a long-running native (in-process) Lua stage** is caught
  via the same `lua_sethook`/`SIGINT` technique `lua.c`'s own standalone
  interpreter uses, scoped to just that stage's execution window. A
  long-running pure-C++ builtin with no Lua call inside it is not
  interruptible this way -- an unsolved v1 edge case, not silently papered
  over.
- **`$?`, `$(cmd)`, `$VAR`, and `${VAR:-default}` are supported.** Command
  substitution runs the inner command line in a forked child exactly like a
  subshell would. `${VAR:-default}` uses the default value if VAR is unset
  or empty.
- **`-c` flag runs Lua, not shell syntax.** `./wisp -c 'print("hi")'` works
  directly -- the argument is evaluated as a Lua expression/statement, same
  as the `:` sigil in interactive mode. Use `-f` for script files.
- **`kill %N` sends SIGTERM** to background job process groups. `jobs -l`
  shows PIDs. Standard job control, nothing exotic.
- **`cd -`** goes to the previous directory (like bash). Tracks the last
  `cd` destination automatically.
- **`source file.lua`** loads and executes a Lua file at runtime. Useful
  for loading function libraries or runtime config.
- **`echo -n`** suppresses trailing newline. **`echo -e`** processes escape
  sequences (`\n`, `\t`, `\\`). Plain `echo` with no flags works as
  expected.

## License

MIT, see `LICENSE`. Vendored `linenoise.c`/`linenoise.h` are BSD-2-Clause,
see `LINENOISE_LICENSE`, Copyright (c) Salvatore Sanfilippo and Pieter
Noordhuis.
