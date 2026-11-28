<div align="center">
  <img src="logo.png" alt="wisp logo" width="110" />
  <h1>wisp</h1>
  <p>A small Linux shell written in C++17 with Lua 5.4 embedded for config, scripting, and structured pipelines.</p>

  <p>
    <a href="https://github.com/hinikaa/wisp/actions/workflows/ci.yml">
      <img src="https://github.com/hinikaa/wisp/actions/workflows/ci.yml/badge.svg" alt="CI" />
    </a>
    <a href="https://github.com/hinikaa/wisp/releases">
      <img src="https://img.shields.io/github/v/release/hinikaa/wisp" alt="Release" />
    </a>
    <a href="LICENSE">
      <img src="https://img.shields.io/github/license/hinikaa/wisp" alt="License" />
    </a>
    <img src="https://img.shields.io/badge/C%2B%2B-17-blue" alt="C++17" />
    <img src="https://img.shields.io/badge/Lua-5.4-000080" alt="Lua 5.4" />
    <img src="https://img.shields.io/badge/platform-Linux-lightgrey" alt="Linux" />
  </p>
</div>

![wisp demo](wisp-demo.gif)

## What is wisp?

wisp is a daily-drivable Linux shell with proper job control, tab completion, inline history autosuggestions, globbing, and reverse history search, except Lua 5.4 replaces bash scripting and config syntax.

Any global function you put in `~/.config/wisp/init.lua` automatically becomes a shell command. When you pipe one Lua function into another, wisp runs them in-process and passes the actual Lua table straight to the next stage instead of turning everything into text and parsing it back out. In a terminal, lists of records render as aligned tables; as soon as you pipe into a normal binary on `$PATH` (like `grep` or `wc`) or redirect to a file, wisp converts them to plain `key=value` lines automatically.

Everything stays dynamically typed with normal Lua values (`string`, `number`, `table`, `boolean`, `nil`). The whole shell is ~3,350 lines of C++17, compiles down to a ~396K stripped binary, and starts faster than bash.

## Install

### Quick install (Linux x86_64 / aarch64)

```sh
curl -fsSL https://raw.githubusercontent.com/Hinikaa/wisp/master/install.sh | sh
```

Release binaries link Lua 5.4 statically so they run across distros without library version mismatches. You can also download `.deb`, `.rpm`, or `.tar.gz` packages directly from the [Releases](https://github.com/hinikaa/wisp/releases) page.

### Arch Linux (AUR)

```sh
yay -S wisp-shell
# or: paru -S wisp-shell
```

### Nix / NixOS

Try without installing:

```sh
nix run github:Hinikaa/wisp
```

### Build from source

You'll need a C++17 compiler and Lua 5.4 development headers discoverable via `pkg-config`:

- **Debian / Ubuntu:** `sudo apt install liblua5.4-dev`
- **Arch:** `sudo pacman -S lua54`

Then build and install:

```sh
make
make test              # unit tests + shell tests + interactive PTY tests
sudo make install      # installs to /usr/local/bin/wisp (override with PREFIX=)
```

If you don't want to use `make`, there's a small Lua build script included:

```sh
lua bootstrap.lua build
lua bootstrap.lua test
lua bootstrap.lua clean
```

To use wisp as your login shell, add `/usr/local/bin/wisp` to `/etc/shells` and run:

```sh
chsh -s /usr/local/bin/wisp
```

## Quick tour

### Structured pipelines out of the box

wisp ships with a small built-in prelude (`files`, `where`, `sort_by`, `cols`, `take`, `skip`, `count`, `lines`, `to_json`), so structured pipelines work right away with zero config:

```text
wisp> files | where type == file | sort_by size desc | take 4 | cols name size
name           size
────────────  ─────
linenoise.c   86505
executor.cpp  51728
lua_env.cpp   24921
main.cpp      13764

wisp> files | where name ~= %.cpp$ | count
5

wisp> files | where name == main.cpp | cols name type | to_json
[{"name":"main.cpp","type":"file"}]
```

Every stage above runs in-process on the Lua stack with no forks. Adding `| wc -l` or `> out.txt` renders the table to text only at that boundary.

### Config (`~/.config/wisp/init.lua`)

Copy [`example-init.lua`](example-init.lua) to `~/.config/wisp/init.lua` (or `$XDG_CONFIG_HOME/wisp/init.lua`) to customize wisp. There's no special alias or config syntax; it's just Lua:

```lua
function prompt()
  local branch = capture("git branch --show-current 2>/dev/null")
  local git = branch ~= "" and (" (" .. branch .. ")") or ""
  local err = wisp.status ~= 0 and (" [" .. wisp.status .. "]") or ""
  return "wisp" .. git .. err .. "> "
end

-- Plain command: first arg is pipeline input (nil if none), rest are shell args
function greet(_, name)
  print("hello, " .. (name or "world"))
end

-- Aliases are just functions calling sh()
function ll()
  sh("ls -la")
end
```

Built-in Lua helpers available in configs and scripts:
- `sh(cmd)` — runs a shell command line and returns its exit code
- `capture(cmd)` — runs a shell command line and returns `(stdout, exit_code)`
- `setenv(key, val)` — sets an environment variable
- `wisp.status` / `wisp.version` — last exit status and shell version

If a Lua function shadows a real binary (for example you define `function ls()`), `command ls` bypasses the Lua function and runs `/usr/bin/ls`.

### Everyday shell usage

Regular shell commands work the way you expect:

```sh
wisp> echo hi > /tmp/f && cat /tmp/f
hi
wisp> ls *.cpp
executor.cpp  lexer.cpp  lua_env.cpp  main.cpp  parser.cpp
wisp> echo {a,b}{1..3}
a1 a2 a3 b1 b2 b3
wisp> FOO=bar env | grep FOO
FOO=bar
wisp> sleep 30 &
[1] 84213
wisp> jobs
[1]  Running    sleep 30
```

Supported shell syntax:
- **Pipelines & chaining:** `|`, `&&`, `||`, `;`, backgrounding with `&`
- **Redirects:** `<`, `>`, `>>`, `2>`, `2>&1`, `&>` (works on builtins too, like `echo hi > f`)
- **Job control:** `Ctrl-Z`, `jobs` (`-l`), `fg`, `bg`, `kill %N` (supports `-9`, `-KILL`, etc.), `disown %N`, `wait`
- **Expansions:** `$VAR`, `${VAR:-default}`, `$?`, `$(cmd)` (including pipes and nesting), globs (`*.cpp`, `src/**/*.h`), brace expansion (`{a,b,c}`, `{1..10}`, `{a..e}`)
- **Quoting & comments:** `'raw'`, `"expands $VAR"`, `\escapes`, `# comments`, and concatenated words like `--format='%h %s'`
- **Env vars:** `FOO=bar` sets it for the session; `FOO=bar cmd` scopes it to a single command (including builtins and Lua functions)

### Interactive Lua mode (`:`)

Prefix any line with `:` to evaluate Lua directly. Expressions print their result automatically, and incomplete blocks prompt with `>>> ` for more lines:

```lua
wisp> :1 + 1
2
wisp> :for i = 1, 3 do
>>>   print("step " .. i)
>>> end
step 1
step 2
step 3
```

### Completion, autosuggestions, history, and typo hints

- **Tab completion** completes builtins, Lua global functions, and `$PATH` executables in command position (including right after `|`, `&&`, `||`, `;`, and `command`), filenames (including `~/...`, with `/` on directories) for arguments, and Lua globals / nested table keys when typing `:prefix` or `:table.prefix`.
- **Inline autosuggestions** show the most recent matching history entry as faint gray ghost text while you type — press `Right` (or `Ctrl-F`) at the end of the line to accept it.
- **`Ctrl-R`** does reverse incremental search through `~/.wisp_history` (`Ctrl-R` for older matches, `Enter` to run, `Esc` to edit, `Ctrl-C` to cancel).
- **Typo suggestions** check `$PATH` executables by edit distance (treating swapped adjacent characters as a single edit):
  ```text
  wisp: grpe: command not found (did you mean 'grep'?)
  ```

### Builtins

| Builtin | What it does |
|---|---|
| `cd [dir \| -]` | Change directory (`cd -` switches back, keeps `$PWD` and `$OLDPWD` updated) |
| `pwd` | Print working directory |
| `echo [-n] [-e]` | Print arguments (`-n` no newline, `-e` escapes like `\n`/`\t`/`\\`, `-ne` combined) |
| `export VAR=val` | Export environment variables |
| `unset VAR [...]` | Remove environment variables |
| `history [N]` | Print command history (all, or the last `N` entries) |
| `source file.lua` | Load and run a Lua file in the current session |
| `type cmd` | Show whether `cmd` is a builtin, Lua function, or `$PATH` executable |
| `command cmd` | Run a `$PATH` executable directly, skipping same-named Lua functions |
| `jobs`, `fg`, `bg`, `kill`, `disown`, `wait` | Standard background job management |
| `pkg` | Thin wrapper around LuaRocks (see below) |
| `exit [code]` | Exit the shell |

### Packages (`pkg`)

Instead of inventing another package manager, `pkg` wraps [LuaRocks](https://luarocks.org) and installs into your local rock tree (`--local`). You just need `luarocks` on `$PATH`:

```sh
wisp> pkg search json
wisp> pkg install luasocket
wisp> pkg list
wisp> pkg info luasocket
wisp> pkg update luasocket
wisp> pkg remove luasocket
```

wisp adds your local rock tree to `package.path` and `package.cpath` at startup, so installed pure-Lua and C rocks work with `require()` right away:

```lua
wisp> :local socket = require("socket"); print(socket._VERSION)
```

### CLI flags & scripts

```sh
wisp                                 # interactive shell
wisp -c 'ls | wc -l'                 # run a shell command (for $SHELL, git aliases, etc.)
wisp -c ':print(42 * 10)'            # leading ':' switches -c to Lua
wisp -e 'for i=1,5 do print(i) end'  # run a Lua chunk directly
wisp -f script.lua foo bar           # run a Lua script (args exposed via Lua's `arg` table)
wisp --version                       # print version
```

Shebang scripts work too (with command-line arguments in `arg[1]`, `arg[2]`, ...):

```lua
#!/usr/bin/env wisp
print("hello", arg[1] or "world")
```

## What wisp doesn't do

Some of this is intentional, some just isn't built yet:

- **No bash control-flow grammar** (`if`/`for`/`while`/`case`). Control flow belongs in Lua, so there's no plan to add shell scripting syntax.
- **No word splitting on variable expansion.** If `$VAR` holds `a b`, it stays one argument (closer to fish than POSIX).
- **No bash arrays (`arr=(1 2 3)`), `[[ ]]`, heredocs (`<<EOF`), `$1`..`$9`, or `${!prefix*}` / `${VAR+alt}`.**
- **No sandboxing of `init.lua`.** It has full Lua stdlib access, same trust model as `.bashrc`.
- **Backgrounding an all-native Lua pipeline (`files | sort_by size &`)** runs in the foreground with a notice, since there's no child process to background.

## Benchmarks

Measured on x86-64 Linux with `g++ 16`, Lua 5.4, bash 5.3, zsh 5.9, and dash 0.5 (`-c ''` with rc files disabled for bash/zsh):

| Metric | wisp | bash 5.3 | zsh 5.9 | dash 0.5 | lua 5.4 |
|---|---:|---:|---:|---:|---:|
| Stripped binary | 396K | 1.1M | ~900K | **~120K** | 318K |
| Source lines | **3,356** | ~500K | ~250K | ~20K | ~25K |
| Startup (best of 40) | 0.55ms | 2.1ms | 1.2ms | 1.2ms | 1.5ms |
| Throughput (500 runs avg) | 1.07ms | 1.66ms | — | — | — |
| Peak RSS | **2.9MB** | 5.7MB | — | — | 2.4MB |

A couple caveats: the machine wasn't completely idle, so treat the relative ordering as more meaningful than exact sub-millisecond numbers. By default the `Makefile` links `libstdc++` and `libgcc` statically, which adds ~200K to the binary size in exchange for avoiding shared-library relocation on every shell spawn. If you prefer a smaller binary, `make STATIC_CXX= LTO=` drops it to ~220K with startup roughly on par with bash.

For structured pipelines, running `nums | sorted | count` over 10,000 records 100 times takes ~430ms, matching `seq 10000 | sort -n | wc -l` through coreutils while keeping records directly indexable as Lua tables.

## How it compares

| Feature | wisp | bash / zsh | dash | fish | nushell | PowerShell |
|---|---|---|---|---|---|---|
| Interactive REPL, hints & Ctrl-R | yes | yes | no | yes | yes | yes |
| Job control (`fg`/`bg`) | yes | yes | no | yes | partial | no |
| Globs & brace expansion | yes | yes | globs | yes | globs | no |
| Config language | **Lua 5.4** | shell DSL | none | fish DSL | Nu | PowerShell |
| Structured pipelines | **Lua tables** | text | text | text | tables | .NET objects |
| POSIX-style interactive syntax | mostly | yes | yes | no | no | no |
| Binary size | **396K** | 1.1M / 900K | ~120K | ~1M | ~15M | 200M+ |

## Under the hood

If you're curious how the codebase is put together, it's five C++ files (`main.cpp`, `lexer.cpp`, `parser.cpp`, `executor.cpp`, `lua_env.cpp`) plus a few headers and vendored `linenoise.c`:

- **Per-character quote masks:** The lexer tracks quoting per character (`0` bare, `1` single-quoted/escaped, `2` double-quoted) rather than per word. That's how `--format='%h %s'` stays a single argument, `\$HOME` stays literal, and `"$HOME"` expands variables without triggering globbing or brace expansion.
- **Zero-copy native pipeline runs:** Consecutive Lua stages pass values directly on the Lua stack and only convert to/from C++ when entering or leaving an external process.
- **Builtin pipelines without deadlocks:** When a builtin feeds a downstream command, wisp captures its output to a temporary file rather than an OS pipe so outputs over 64KB can't deadlock before the reader starts.
- **Interrupting Lua with `Ctrl-C`:** Long-running in-process Lua stages install a `SIGINT` + `lua_sethook` handler (the same trick standalone `lua.c` uses) so `Ctrl-C` stops the script without killing the shell. (A long-running pure-C++ builtin without Lua calls isn't interruptible this way yet.)
- **Cached `$PATH` index:** Tab completion, `type`, and command-not-found hints share one cached scan of `$PATH` that invalidates automatically if `PATH` changes.
- **Three-layer test suite:** `make test` runs four C++ unit test binaries (lexer, parser, executor, Lua boundary), an end-to-end script (`test_shell.sh`), and a Python PTY harness (`test_interactive.py`) that exercises real terminal job control.

*(Full disclosure: I used AI to help write the C++ parts since C++ isn't my strongest language, while steering the design, testing, and debugging myself.)*

## License & credits

MIT License — see [`LICENSE`](LICENSE).

Line editing uses vendored [`linenoise`](https://github.com/antirez/linenoise) (BSD-2-Clause, see [`LINENOISE_LICENSE`](LINENOISE_LICENSE)), copyright Salvatore Sanfilippo and Pieter Noordhuis, with small local patches for `Ctrl-R` reverse incremental search, Right-arrow hint acceptance, and read-only history access.
