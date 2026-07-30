# wisp

A Linux shell with real job control that uses Lua -- not a bespoke DSL -- as
its scripting and config language, and passes structured data (Lua tables)
between pipeline stages instead of just text.

![wisp demo](wisp-demo.gif)

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
make test              # unit + end-to-end + pty tests
sudo make install      # /usr/local/bin/wisp, PREFIX= to change
```

Or use the self-contained bootstrap script, which needs no `make`:

```
lua bootstrap.lua build   # compile wisp
lua bootstrap.lua test    # run all tests
lua bootstrap.lua clean   # remove build artifacts
```

To use wisp as your login shell, add the installed path to `/etc/shells`
and `chsh -s /usr/local/bin/wisp`.

### Tests

`make test` runs three layers:

- four unit binaries over the pure pieces (lexer, parser, segment grouping,
  the Lua value boundary)
- `test_shell.sh`, which feeds lines to the real `./wisp` and diffs what
  comes back
- `test_interactive.py`, which drives it through an actual pty

Each layer exists because the one above it missed something real. Unit tests
over a pure lexer pass happily while the shell they belong to drops every
builtin's output on the floor. And piping into stdin means `isatty()` is
false, so the entire job-control path never runs -- both of the
session-killing bugs found so far lived exactly there, invisible to
everything but a pty.

## Usage

```
./wisp
```

Or run a one-off command line, the way anything spawning a `$SHELL` does:

```
./wisp -c 'ls | wc -l'
./wisp -c 'make && ./wisp -c "echo built"'
```

`-c` is shell syntax. A leading `:` makes it Lua, exactly as at the prompt,
and `-e` takes Lua directly:

```
./wisp -c ':print(42 * 10)'
./wisp -e 'for i=1,5 do print(i) end'
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
`make && ./wisp`, `sleep 30 &`, redirects (`<` `>` `>>` `2>` `2>&1` `&>`),
`Ctrl-Z`/`fg`/`bg` for job control.

Quoting works the way the muscle memory expects: `'single'` is raw,
`"double"` still expands `$VAR`, a backslash escapes the next character
(`touch my\ file`, `echo \$PATH`), an unquoted `#` starts a comment, and
adjacent chunks are one argument -- `--format='%h %s'` reaches the program
whole rather than split at the quote.

Environment assignments work in both positions:

```
wisp> FOO=bar             # sets it for the session
wisp> FOO=bar env | grep FOO   # sets it for that command only
```

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

`example-init.lua` in this repo is a working starting point -- copy it there
and edit.

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
  `jobs`, `fg`, `bg`, `kill`, `source`, `echo`, `disown`, `wait`, `pwd`,
  `type`, `pkg`), Lua global functions, and `$PATH` executables (filtered to
  `+x` only). The `$PATH` scan is cached and shared with `type` and the
  command-not-found hint, so it costs one walk per session, not one per Tab
- **Filenames** (later words): with `/` suffix for directories, hidden files
  shown only when prefix starts with `.`
- **Lua mode** (`:prefix`): all Lua global names when input starts with `:`

**Ctrl-R** is reverse incremental history search, as everywhere else: type to
narrow, Ctrl-R again for the next older match, Enter to run it, Esc to edit
it first, Ctrl-C to go back to what you were typing.

### Error messages

wisp gives context-aware error messages:

```
wisp: grpe: command not found (did you mean 'grep'?)
wisp: expected a filename after redirect, got |
wisp: unexpected end of input after redirect
wisp: lua: [string "1+"]:1: unexpected symbol near '1'
```

Command-not-found suggestions score every `$PATH` executable by edit distance
(threshold: 3), counting a swapped pair of adjacent characters as one edit
rather than two -- which is what a mistyped command usually is, so `grpe`
reaches `grep` instead of tying with a dozen unrelated names.

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

### Package Management

`pkg` is a front end to [LuaRocks](https://luarocks.org), not a package
manager of its own -- wisp has no business reinventing one, and Lua already
has the ecosystem. It requires `luarocks` on `$PATH` and always installs
`--local`.

```
wisp> pkg search json          # luarocks search
wisp> pkg install luasocket    # luarocks install --local
wisp> pkg list                 # luarocks list
wisp> pkg info luasocket       # luarocks show
wisp> pkg update luasocket     # reinstall at the latest version
wisp> pkg remove luasocket     # luarocks remove
```

Installed rocks are available via `require()` immediately: wisp puts the
local rock tree on both `package.path` and `package.cpath` at startup, so
pure-Lua and C rocks both load.

```lua
local socket = require("socket")
print(socket._VERSION)
```

## Benchmarks

Measured on x86-64 Linux, g++ 15, Lua 5.4, bash 5.3, zsh 5.9, dash 0.5.

| | wisp | bash 5.3 | zsh | dash | lua 5.4 |
|---|---|---|---|---|---|
| Binary, stripped | 423K | 1.1M | ~900K | **~120K** | 318K |
| Source lines | **2,533** | ~500K | ~250K | ~20K | ~25K |
| Startup, best of 40 | 1.6ms | 2.1ms | **1.2ms** | **1.2ms** | 1.5ms |
| Throughput, 500 runs | 1.07ms | 1.66ms | -- | -- | -- |
| Peak RSS | **2.9MB** | 5.7MB | -- | -- | 2.4MB |

Startup and throughput are `-c ''` (bash and zsh with rc files disabled),
on a machine that was not idle -- treat the absolute milliseconds as noise
and the ordering as the result.

wisp starts and runs faster than bash while using half the memory and a
third of the binary, and stays behind dash and zsh, which is the honest
place to be: it pays for `luaL_openlibs` up front, and that is exactly what
buys a real language at the prompt. If sub-millisecond startup is the
requirement, dash is right there and wisp is not trying to beat it.

Most of wisp's margin over bash comes from linking libstdc++ and libgcc
statically -- a shell is spawned constantly, and not mapping and relocating
a 2MB shared object every time is worth more than the ~200K it adds to the
binary. `make STATIC_CXX= LTO=` opts out and gives a ~180K binary at bash's
startup speed.

**Pipelines.** Values crossing between native (in-process) stages stay
inside the Lua VM for the whole run and are converted to a C++
representation only at the two ends -- once on the way in from an external
process, once on the way out. A `nums | sorted | count` pipeline over
10,000 records, 100 times, runs in ~430ms, against ~430ms for the same
shape as `seq 10000 | sort -n | wc -l` through coreutils. Parity with
optimized C for a Lua `table.sort` is the honest claim here; the win is
that the records arrive as tables you can index, not as text you have to
re-parse at every stage.

## Comparison with other shells

Not everyone needs bash. Here's where wisp sits relative to the alternatives
people actually use.

| Feature | wisp v2 | bash/zsh | dash/busybox sh | fish | nushell/elvish | powershell |
|---|---|---|---|---|---|---|
| Interactive REPL | yes | yes | no | yes | yes | yes |
| Reverse history search (Ctrl-R) | yes | yes | no | yes | yes | yes |
| Job control (fg/bg) | yes | yes | no | yes | partial | no |
| Globbing | yes | yes | yes | yes | yes | no |
| Tab completion | yes | yes | no | yes | yes | yes |
| Config via real language | **Lua** | bash DSL | no | fish DSL | own lang | PS scripts |
| Structured data in pipelines | **Lua tables** | no (text) | no (text) | no (text) | yes | .NET objects |
| Alias via functions | **auto** | manual | no | functions | functions | functions |
| Script execution | `-c`/`-e`/`-f` | full | full | full | full | full |
| Shebang support | yes | yes | yes | yes | yes | no |
| `$?` / `$(cmd)` | yes | yes | yes | `$status` | `$env:` | `$LASTEXITCODE` |
| `${VAR:-default}` | yes | yes | yes | yes | yes | no |
| Brace expansion | yes | yes | no | yes | yes | no |
| External command not-found | **Levenshtein hint** | no | no | no | no | no |
| Binary size | **423K** | 1.1M | ~120K | ~1M | ~15M | ~200M+ |
| Source lines | **2,533** | ~500K | ~20K | ~250K | ~100K | millions |

### What wisp v2 can do

- Run any shell command: `ls`, `grep -r src/`, `make && make install`
- Be your login shell: `-c` takes shell syntax, so anything that spawns
  `$SHELL` (git aliases, `xdg-open`, `system()`, your terminal emulator)
  works
- Redirects: `<`, `>`, `>>`, `2>`, `2>&1`, `&>` -- on builtins too, so
  `echo hi > f` and `jobs 2>/dev/null` behave
- Background jobs: `sleep 30 &` then `jobs`, `fg %1`, `bg %1`
- Kill background jobs: `kill %1`, `kill -9 %1`
- Disown jobs: `disown %1` (removes from job table, survives shell exit)
- Wait for jobs: `wait %1` or `wait` (all background jobs)
- `$?`, `$(cmd)`, `$VAR`, `${VAR:-default}` expansion -- including
  `$(cmd | with | pipes)` and nested `$(echo $(date))`
- Environment assignments: `FOO=bar` for the session, `FOO=bar cmd` for one
  command
- Brace expansion: `{a,b,c}`, `{x,y}{1,2}`, ranges `{1..10}` and `{a..e}`
- Glob patterns: `*.cpp`, `src/**/*.h`
- Backslash escapes and `#` comments
- Tab completion: commands, filenames, Lua functions
- Lua one-liners: `:1 + 1`, `:print("hi")`
- Lua config: define `prompt()`, aliases, pipeline stages in `init.lua`
- Structured pipelines: `nums | sorted | wc -l` passes Lua tables between stages
- One-off commands: `./wisp -c 'ls | wc -l'`, `./wisp -e 'return {1, 2, 3}'`
- Script execution: `./wisp -f script.lua` or shebang `#!/usr/bin/env wisp`
- `cd -` to go to previous directory
- `pwd` to print working directory
- `source file.lua` to load Lua files at runtime
- `type cmd` to check if command is builtin, function, or PATH executable
- `echo -n` (no newline) and `echo -e` (escape sequences: `\n`, `\t`, `\\`)
- `pkg install|remove|list|search|info|update` on top of LuaRocks
- Error hints: `wisp: grpe: command not found (did you mean 'grep'?)`

### What wisp v2 can't do (yet)

- No array syntax: `arr=(1 2 3)` doesn't exist
- No `[[ ]]` double-bracket tests
- No `${!prefix*}` or `${VAR+alternative}` expansions
- No heredocs (`<<EOF`), no `$1`..`$9` positional parameters
- No word splitting on expansion results: `$VAR` holding `a b` is one
  argument, not two. Closer to fish than to POSIX, and deliberate
- A backslash-escaped glob character still globs: `echo \*` expands.
  Quote it (`echo '*'`) to suppress
- Control flow is Lua's job -- there is no `if`/`while`/`for` shell grammar,
  and there is not going to be one

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
  POSIX compatibility. wisp is 423K and runs on any Linux box.

## "But it's just AI slop"

No.

- **~2,500 lines of C++17**, not a framework. No React, no "hello world" boilerplate,
  no dependency tree. `g++ -std=c++17` and a Lua pkg-config flag. That's it.
- **Lexer, parser, executor, Lua embedder** -- hand-written, not generated.
  The parser is a recursive descent with actual error recovery (`<eof>`
  continuation, Levenshtein suggestions), not a grammar dump.
- **Structured pipeline data** is a real design choice, not a ChatGPT prompt.
  Passing Lua tables between stages means zero fork/serialize overhead for
  native runs -- a deliberate tradeoff against the "everything is bytes"
  model every other shell uses.
- **423K binary** after strip, and it starts faster than bash. Five C++
  files and a Makefile, not a "vibe-coded" project with 47 config files and a CI
  pipeline longer than the source.
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
data in pipelines. It does it in about 2,500 lines, with an end-to-end test
suite that runs the real binary. That's not slop. That's focus.

**Full disclosure:** I had trouble with C++ and used AI for assistance.
That's not the same as "AI wrote this." I made the design decisions --
Lua tables in pipelines, the parser structure, the job control model.
The code went through review, testing, and real debugging. AI helped me
write C++ I couldn't write alone. That's a tool, not a ghostwriter.

## Notes

- **Globbing works for bare and double-quoted words.** Single-quoted words
  are never expanded. Patterns matching nothing are kept as-is (bash
  behavior). Brace expansion is separate from globbing and works on all
  unquoted words.
- **Quoting is tracked per character, not per word.** The lexer carries a
  mask alongside each word saying which characters came from quotes or a
  backslash, which is why `--format='%h %s'` stays one argument and why
  `\$HOME` survives expansion while `"$HOME"` does not.
- **A native run's value never leaves the Lua VM.** Consecutive
  function/builtin stages chain on the Lua stack; the C++ `LuaValue`
  representation is built once on the way in from an external process and
  once on the way out to one. Converting between stages would deep-copy
  every record twice per stage, which would defeat the point.
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
  subshell would, and the tokenizer carries `$( ... )` through whole, so
  pipes, spaces and nesting inside it belong to the inner line. A `)` inside
  quotes inside a substitution (`$(echo ")")`) is past what a
  non-recursive scan can see.
- **`-c` runs shell syntax, `-e` runs Lua.** `-c` is what `system()`, git
  aliases and terminal emulators call, so it has to be the shell one; a
  leading `:` still switches a `-c` argument to Lua, exactly like the prompt.
  Use `-f` for script files.
- **Builtins participate in pipelines and redirects properly.** `echo hi |
  wc -c` pipes, `echo hi > f` writes the file. A builtin feeding a later
  stage has its output captured into a temp file rather than an OS pipe --
  nothing is draining the far end yet, and a pipe would deadlock past 64K.
- **`kill %N` sends SIGTERM** to background job process groups. `jobs -l`
  shows PIDs. Standard job control, nothing exotic.
- **`cd -`** goes to the previous directory (like bash). Tracks the last
  `cd` destination automatically.
- **`source file.lua`** loads and executes a Lua file at runtime. Useful
  for loading function libraries or runtime config.
- **`echo -n`** suppresses trailing newline. **`echo -e`** processes escape
  sequences (`\n`, `\t`, `\\`). Plain `echo` with no flags works as
  expected.
- **`disown %N`** removes a job from the job table. The process keeps
  running but the shell won't send SIGHUP on exit.
- **`wait %N`** blocks until the specified background job finishes. `wait`
  with no arguments waits for all background jobs.
- **`pwd`** prints the current working directory.
- **`type cmd`** shows whether a command is a shell builtin, a Lua function,
  or a PATH executable (and where).
- **`pkg` requires LuaRocks** and always installs `--local`. There is no
  fallback mode: a package manager that can't fetch packages isn't one.
  Rocks land on `package.path` and `package.cpath`, so `require()` finds
  them straight away. Arguments are passed to `luarocks` via `execvp`, not
  through `/bin/sh`.
- **Brace expansion** works: `{a,b,c}` expands to separate words,
  `{x,y}{1,2}` to `x1 x2 y1 y2`, `{1..10}` and `{a..e}` are ranges. Nested
  braces supported. A group with neither a comma nor a range is *not* an
  expansion and is left exactly as written, so `git show HEAD@{1}` and
  `awk {print}` survive unquoted.
- **`2>&1`** redirects stderr to stdout, **`&>file`** sends both to a file.
- **Ctrl-C at the prompt abandons the line**, sets `$?` to 130, and gives
  you a new prompt. It does not end the session -- linenoise reports it the
  same way it reports EOF, which is a trap worth knowing about if you embed
  it yourself.
- **`$PWD` and `$OLDPWD` track `cd`**, so a `prompt()` reading them, and any
  child process, sees the truth.
- **`kill %N` on a stopped job sends SIGCONT after the signal.** A suspended
  process never runs long enough to act on a SIGTERM otherwise.

## License

MIT, see `LICENSE`. Vendored `linenoise.c`/`linenoise.h` are BSD-2-Clause,
see `LINENOISE_LICENSE`, Copyright (c) Salvatore Sanfilippo and Pieter
Noordhuis.

`linenoise.c` carries one local change: reverse incremental search (Ctrl-R),
which upstream does not have. It is marked in the source and confined to a
single function plus one `switch` case, so it stays easy to spot and to
re-apply if the vendored copy is ever updated.
