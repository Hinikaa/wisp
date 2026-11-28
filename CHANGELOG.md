# Changelog

## v3.3.0

- **Structured sources & parsers:** Added native `procs` (`/proc` on Linux, `libproc` on macOS), `from_json`, and `from_csv` to the built-in prelude.
- **Void Lua command exit status fix:** Lua functions that return `nil` (like void aliases `ll` or `greet`) now exit with status `0` instead of `1`; explicit `false` or Lua errors still exit `1`.
- **Smarter typo suggestions:** Command-not-found hints now check builtins and Lua functions alongside `$PATH` executables and break ties using command history frequency (`fiels` -> `files`, `exi` -> `exit`).
- **macOS support:** Added macOS CI, static-Lua `wisp-macos-arm64.tar.gz` release builds, macOS support in `install.sh` and `flake.nix`, and a Homebrew tap (`brew install hinikaa/tap/wisp`).
- **Recipes:** Added `examples/recipes.lua` (`top_mem`, `git_log`, `json_get`, `ext_counts`).

## v3.2.1

- Added `.deb` and `.rpm` release packages (`x86_64` and `aarch64`), `install.sh`, `flake.nix`, and refreshed demo GIF.

## v3.2.0

- Built-in structured pipeline prelude (`files`, `where`, `sort_by`, `cols`, `take`, `skip`, `count`, `lines`, `to_json`) and column-aligned TTY table rendering.
- Fish-style inline history autosuggestions (accept with `Right` or `Ctrl-F`) and post-pipe/post-operator tab completion.
- Added `unset` and `history` builtins, Lua `capture(cmd)` and `wisp.status` / `wisp.version`, and `arg` population for Lua scripts.
- Switched release binaries to statically linked Lua 5.4 across `x86_64` and `aarch64`, and published `wisp-shell` on the AUR.

## v3.1.0

- Fixed tab-completion command duplication, per-character quoting protection for globs/braces, and `-c` hang on a PTY with `SIGTTIN` ignored.

## v3.0.0

- Rebuilt core shell architecture (~2.8k lines C++17 + Lua 5.4), real POSIX job control, `Ctrl-R` history search, `pkg` LuaRocks wrapper, and three-layer test suite.
