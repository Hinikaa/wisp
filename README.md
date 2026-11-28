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

wisp is a daily-drivable Linux shell with proper job control, tab completion, globbing, and history search, except Lua 5.4 replaces bash scripting and config syntax.

Any global function you put in `~/.config/wisp/init.lua` automatically becomes a shell command. Better yet, when you pipe one Lua function into another, wisp runs them in-process and passes the actual Lua table straight to the next stage instead of turning everything into text and parsing it back out. As soon as a pipeline hits a normal binary on `$PATH` (like `grep` or `wc`), wisp forks and converts the table to plain text at that boundary automatically.

Everything stays dynamically typed with normal Lua values (`string`, `number`, `table`, `boolean`, `nil`). The whole shell is ~2,860 lines of C++17, compiles down to a ~344K stripped binary, and starts faster than bash.

## Install

### Prebuilt binary

Grab `wisp-linux-x86_64.tar.gz` from the [Releases](https://github.com/hinikaa/wisp/releases) page, extract it, and put `wisp` somewhere on your `$PATH`.

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

### Config and structured pipelines

Copy [`example-init.lua`](example-init.lua) to `~/.config/wisp/init.lua` (or `$XDG_CONFIG_HOME/wisp/init.lua`) to get started. There's no special alias or config syntax; it's just Lua:

```lua
function prompt()
  return "wisp> "
end

-- Plain command: first arg is pipeline input (nil if none), rest are shell args
function greet(_, name)
  print("hello, " .. (name or "world"))
end

-- Aliases are just functions calling sh()
function ll()
  sh("ls -la")
end

-- Pipeline stages pass real Lua tables in-process
function nums()
  return { { n = 3 }, { n = 1 }, { n = 2 } }
end

function sorted(input)
  table.sort(input, function(a, b) return a.n < b.n end)
  return input
end
```

At the prompt:

```text
wisp> greet world
hello, world
wisp> nums | sorted
n=1
n=2
n=3
wisp> nums | sorted | wc -l
3
```

In `nums | sorted`, neither stage forks; `sorted` gets the exact Lua table returned by `nums`. Adding `| wc -l` renders the rows to text only when handing off to `wc`.

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

### Tab completion, history, and typo hints

- **Tab completion** completes builtins, Lua global functions, and `$PATH` executables for the first word, filenames (including `~/...`, with `/` on directories) for arguments, and Lua globals / nested table keys when typing `:prefix` or `:table.prefix`.
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
wisp                            # interactive shell
wisp -c 'ls | wc -l'            # run a shell command (for $SHELL, git aliases, etc.)
wisp -c ':print(42 * 10)'       # leading ':' switches -c to Lua
wisp -e 'for i=1,5 do print(i) end'  # run a Lua chunk directly
wisp -f script.lua              # run a Lua script file
wisp --version                  # print version
```

Shebang scripts work too:

```lua
#!/usr/bin/env wisp
print("hello from script")
```

## What wisp doesn't do

Some of this is intentional, some just isn't built yet:

- **No bash control-flow grammar** (`if`/`for`/`while`/`case`). Control flow belongs in Lua, so there's no plan to add shell scripting syntax.
- **No word splitting on variable expansion.** If `$VAR` holds `a b`, it stays one argument (closer to fish than POSIX).
- **No bash arrays (`arr=(1 2 3)`), `[[ ]]`, heredocs (`<<EOF`), `$1`..`$9`, or `${!prefix*}` / `${VAR+alt}`.**
- **No sandboxing of `init.lua`.** It has full Lua stdlib access, same trust model as `.bashrc`.
- **Backgrounding an all-native Lua pipeline (`nums | sorted &`)** runs in the foreground with a notice, since there's no child process to background.

## Benchmarks

Measured on x86-64 Linux with `g++ 16`, Lua 5.4, bash 5.3, zsh 5.9, and dash 0.5 (`-c ''` with rc files disabled for bash/zsh):

| Metric | wisp | bash 5.3 | zsh 5.9 | dash 0.5 | lua 5.4 |
|---|---:|---:|---:|---:|---:|
| Stripped binary | 344K | 1.1M | ~900K | **~120K** | 318K |
| Source lines | **2,864** | ~500K | ~250K | ~20K | ~25K |
| Startup (best of 40) | 0.55ms | 2.1ms | 1.2ms | 1.2ms | 1.5ms |
| Throughput (500 runs avg) | 1.07ms | 1.66ms | — | — | — |
| Peak RSS | **2.9MB** | 5.7MB | — | — | 2.4MB |

A couple caveats: the machine wasn't completely idle, so treat the relative ordering as more meaningful than exact sub-millisecond numbers. By default the `Makefile` links `libstdc++` and `libgcc` statically, which adds ~200K to the binary size in exchange for avoiding shared-library relocation on every shell spawn. If you prefer a smaller binary, `make STATIC_CXX= LTO=` drops it to ~180K with startup roughly on par with bash.

For structured pipelines, running `nums | sorted | count` over 10,000 records 100 times takes ~430ms, matching `seq 10000 | sort -n | wc -l` through coreutils while keeping records directly indexable as Lua tables.

## How it compares

| Feature | wisp | bash / zsh | dash | fish | nushell | PowerShell |
|---|---|---|---|---|---|---|
| Interactive REPL & Ctrl-R | yes | yes | no | yes | yes | yes |
| Job control (`fg`/`bg`) | yes | yes | no | yes | partial | no |
| Globs & brace expansion | yes | yes | globs | yes | globs | no |
| Config language | **Lua 5.4** | shell DSL | none | fish DSL | Nu | PowerShell |
| Structured pipelines | **Lua tables** | text | text | text | tables | .NET objects |
| POSIX-style interactive syntax | mostly | yes | yes | no | no | no |
| Binary size | **344K** | 1.1M / 900K | ~120K | ~1M | ~15M | 200M+ |

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

Line editing uses vendored [`linenoise`](https://github.com/antirez/linenoise) (BSD-2-Clause, see [`LINENOISE_LICENSE`](LINENOISE_LICENSE)), copyright Salvatore Sanfilippo and Pieter Noordhuis, with one small local patch adding `Ctrl-R` reverse incremental history search.
