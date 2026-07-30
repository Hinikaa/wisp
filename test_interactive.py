#!/usr/bin/env python3
"""Interactive tests: drive wisp through a real pty.

test_shell.sh pipes stdin, which means isatty() is false and the entire
job-control path never runs. Every session-killing bug found so far lived
exactly there -- Ctrl-C being indistinguishable from EOF, and a command
substitution handing the terminal to a subshell that never gave it back --
and neither was visible to any other test. Hence this file.

Skips itself (exit 0) if a pty cannot be allocated.
"""

import fcntl
import os
import pty
import re
import select
import struct
import sys
import termios
import time

WISP = os.path.join(os.path.dirname(os.path.abspath(__file__)), "wisp")
ESCAPES = re.compile(r"\x1b\[[0-9;?]*[a-zA-Z]")


def output_lines(raw):
    """Lines wisp printed, with the terminal's echo of our keystrokes removed.

    This matters more than it sounds: a pty echoes everything typed, so a
    naive `"alive" in out` passes even when the shell died before running
    `echo alive` -- the echoed keystrokes contain the word. Segments carrying
    a prompt are echo; the rest is output, matched whole rather than by
    substring.
    """
    out = []
    for seg in re.split(r"[\r\n]+", raw):
        seg = seg.strip()
        if not seg or "wisp>" in seg or seg.startswith(">>>"):
            continue
        # "^C"/"^Z" are the tty rendering the control character we sent, not
        # anything wisp wrote.
        seg = re.sub(r"^\^[A-Z]", "", seg).strip()
        if seg:
            out.append(seg)
    return out


def run(steps, home):
    """Send keystrokes to a fresh interactive wisp; return what it printed."""
    pid, fd = pty.fork()
    if pid == 0:
        os.environ["HOME"] = home
        os.environ["XDG_CONFIG_HOME"] = os.path.join(home, "config")
        os.execv(WISP, [WISP])
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))
    out = b""
    for delay, data in steps:
        time.sleep(delay)
        if data:
            os.write(fd, data)
        end = time.time() + 0.4
        while time.time() < end and select.select([fd], [], [], 0.1)[0]:
            try:
                chunk = os.read(fd, 4096)
            except OSError:
                break
            if not chunk:
                break
            out += chunk
    for cleanup in (lambda: os.close(fd), lambda: os.waitpid(pid, 0)):
        try:
            cleanup()
        except OSError:
            pass
    return ESCAPES.sub("", out.decode(errors="replace"))


def main():
    try:
        home = os.environ.get("TMPDIR", "/tmp")
        home = os.path.join(home, "wisp-itest-%d" % os.getpid())
        os.makedirs(home, exist_ok=True)
        probe = run([(0.4, None), (0.2, b"exit\r")], home)
    except OSError as e:
        print("test_interactive: skipped (no pty: %s)" % e)
        return 0
    if "wisp>" not in probe:
        print("test_interactive: skipped (no usable pty)")
        return 0

    seed = [(0.4, None)] + [
        (0.25, c) for c in (b"echo apple-pie\r", b"echo banana-split\r", b"echo cherry-tart\r")
    ]

    cases = [
        # Ctrl-C abandons the line. linenoise reports it exactly like EOF, so
        # a shell that trusts a NULL return quits here instead.
        ("ctrl-c keeps the session",
         [(0.4, None), (0.2, b"echo abandoned"), (0.2, b"\x03"),
          (0.3, b"echo still-here\r"), (0.3, b"exit\r")],
         ["still-here"], ["abandoned"]),
        # A subshell must not hand the terminal to its own child and walk away:
        # the shell would then read from a background process group, get EIO,
        # and mistake it for end of input. Needs a real external command --
        # a builtin never forks and never reproduces it.
        ("command substitution keeps the session",
         [(0.4, None), (0.3, b"echo [$(uname -s)]\r"), (0.3, b"echo after-subst\r"), (0.3, b"exit\r")],
         ["after-subst"], []),
        # SIGINT is ignored while job control owns it, so without the Lua
        # debug hook a runaway chunk wedges the shell for good.
        ("ctrl-c interrupts runaway lua",
         [(0.4, None), (0.2, b":while true do end\r"), (0.9, b"\x03"),
          (0.4, b"echo recovered\r"), (0.3, b"exit\r")],
         ["wisp: lua: interrupted", "recovered"], []),
        ("ctrl-z stops a job, kill reaps it",
         [(0.4, None), (0.2, b"sleep 20\r"), (0.5, b"\x1a"), (0.4, b"jobs\r"), (0.3, b"kill %1\r"),
          (0.5, b"jobs\r"), (0.3, b"exit\r")],
         ["[1]  Stopped    sleep 20", "[1]+ Done    sleep 20"], []),
        ("ctrl-r searches history and runs the match",
         seed + [(0.3, b"\x12"), (0.4, b"ban"), (0.4, b"\r"), (0.4, b"exit\r")],
         ["banana-split"], []),
        ("ctrl-r then ctrl-c restores the typed line",
         seed + [(0.3, b"echo KEEPME"), (0.3, b"\x12"), (0.4, b"apple"), (0.3, b"\x03"),
                 (0.4, b"\r"), (0.4, b"exit\r")],
         ["KEEPME"], []),
        ("tab completes a builtin",
         [(0.4, None), (0.2, b"ech\t"), (0.4, b" completed\r"), (0.3, b"exit\r")],
         ["completed"], []),
    ]

    failed = 0
    for name, steps, wants, unwanted in cases:
        raw = run(steps, home)
        lines = output_lines(raw)
        missing = [w for w in wants if w not in lines]
        present = [u for u in unwanted if u in lines]
        if missing or present:
            failed += 1
            print("FAIL %s" % name)
            for w in missing:
                print("  missing line: %r" % w)
            for u in present:
                print("  unexpected line: %r" % u)
            print("  got: %r" % (lines[-6:],))

    print("test_interactive: %d passed, %d failed" % (len(cases) - failed, failed))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
