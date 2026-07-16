# wisp

A Linux shell with real job control that uses Lua -- not a bespoke DSL -- as
its scripting and config language, and passes structured data (Lua tables)
between pipeline stages instead of just text.

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

## Usage

```
./wisp
```

Bare lines are ordinary shell syntax: `ls -la`, `cd ..`, `cat file | grep foo`,
`make && ./wisp`, `sleep 30 &`, redirects (`<` `>` `>>` `2>`), `Ctrl-Z`/`fg`/
`bg`/`jobs` for job control.

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
wisp> hello claude
hello, claude
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

## Notes

- **Not POSIX-compliant, on purpose.** No globbing, no `${...}` parameter
  expansion, no arrays/arithmetic, no brace expansion, no here-docs, no
  subshell `(...)` grouping. Quoting is simplified: a word is either fully
  single-quoted (literal, never expanded) or fully bare/double-quoted
  (subject to `$VAR`/`$(cmd)` expansion) -- no concatenating quoted and
  unquoted fragments within one word.
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
- **`$?` and `$(cmd)` are supported; `${VAR:-default}`-style expansions are
  not.** Command substitution runs the inner command line in a forked
  child exactly like a subshell would.
- Not a `bash`/`zsh` replacement for scripts -- there's no script-file or
  shebang execution mode in v1, interactive use only.

## License

MIT, see `LICENSE`. Vendored `linenoise.c`/`linenoise.h` are BSD-2-Clause,
see `LINENOISE_LICENSE`, Copyright (c) Salvatore Sanfilippo and Pieter
Noordhuis.
