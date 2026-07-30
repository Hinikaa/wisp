#include "executor.h"

#include "builtins.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <glob.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

bool ShellJob::all_completed() const {
    if (procs.empty()) return false;
    for (auto& p : procs)
        if (!p.completed) return false;
    return true;
}

bool ShellJob::all_stopped() const {
    // "Stopped" means nothing is still running, and at least one process
    // was actually suspended (Ctrl-Z) -- not merely that every process
    // happens to be completed, which trivially satisfies "nothing running"
    // too but isn't what a job-control "Stopped" notice means.
    if (procs.empty()) return false;
    bool any_stopped = false;
    for (auto& p : procs) {
        if (!p.completed && !p.stopped) return false;
        if (p.stopped) any_stopped = true;
    }
    return any_stopped;
}

std::string format_job_line(const ShellJob& j) {
    const char* state = j.all_stopped() ? "Stopped" : (j.all_completed() ? "Done" : "Running");
    return "[" + std::to_string(j.id) + "]  " + state + "    " + j.cmdline;
}

namespace {

// `{a..e}` / `{1..9}`, ascending or descending. Returns false when the group
// isn't a range at all, which is the caller's cue to try commas.
bool expand_range(const std::string& inner, std::vector<std::string>& out) {
    size_t dots = inner.find("..");
    if (dots == std::string::npos || inner.find("..", dots + 2) != std::string::npos) return false;
    std::string lo = inner.substr(0, dots), hi = inner.substr(dots + 2);
    if (lo.empty() || hi.empty()) return false;

    if (lo.size() == 1 && hi.size() == 1 && std::isalpha(static_cast<unsigned char>(lo[0])) &&
        std::isalpha(static_cast<unsigned char>(hi[0]))) {
        int step = lo[0] <= hi[0] ? 1 : -1;
        for (char ch = lo[0];; ch = static_cast<char>(ch + step)) {
            out.push_back(std::string(1, ch));
            if (ch == hi[0]) break;
        }
        return true;
    }

    char* end = nullptr;
    long a = std::strtol(lo.c_str(), &end, 10);
    if (*end) return false;
    long b = std::strtol(hi.c_str(), &end, 10);
    if (*end) return false;
    // A range is the one place a typo can hang the shell; 100k words is far
    // past anything deliberate.
    if (std::labs(b - a) > 100000) return false;
    for (long v = a;; v += (a <= b ? 1 : -1)) {
        out.push_back(std::to_string(v));
        if (v == b) break;
    }
    return true;
}

// Depth-0 comma split of a brace group's contents.
bool split_alternatives(const std::string& inner, std::vector<std::string>& out) {
    std::string current;
    int depth = 0;
    bool saw_comma = false;
    for (char c : inner) {
        if (c == '{') ++depth;
        else if (c == '}') --depth;
        if (c == ',' && depth == 0) {
            saw_comma = true;
            out.push_back(current);
            current.clear();
        } else {
            current += c;
        }
    }
    if (!saw_comma) return false;
    out.push_back(current);
    return true;
}

} // namespace

std::vector<std::string> brace_expand(const std::string& s) {
    size_t open = s.find('{');
    if (open == std::string::npos) return {s};

    size_t close = std::string::npos;
    int depth = 1;
    for (size_t i = open + 1; i < s.size(); ++i) {
        if (s[i] == '{') ++depth;
        else if (s[i] == '}') { --depth; if (depth == 0) { close = i; break; } }
    }
    if (close == std::string::npos) return {s};

    std::string before = s.substr(0, open);
    std::string inner = s.substr(open + 1, close - open - 1);
    std::string after = s.substr(close + 1);

    // No comma and no range means this isn't a brace expansion at all --
    // `git show HEAD@{1}` and `awk {print}` have to come through unchanged.
    std::vector<std::string> items;
    if (!split_alternatives(inner, items) && !expand_range(inner, items)) {
        auto rest = brace_expand(after);
        for (auto& r : rest) r = before + "{" + inner + "}" + r;
        return rest;
    }

    std::vector<std::string> result;
    for (auto& item : items) {
        auto expanded = brace_expand(before + item + after);
        result.insert(result.end(), expanded.begin(), expanded.end());
    }
    return result;
}

std::vector<Segment> build_segments(const std::vector<ExpCmd>& cmds) {
    std::vector<Segment> segs;
    Segment cur;
    cur.native = true;
    auto flush = [&] {
        if (!cur.idx.empty()) { segs.push_back(cur); cur.idx.clear(); }
    };
    for (size_t i = 0; i < cmds.size(); ++i) {
        bool native = cmds[i].kind == CmdKind::Builtin || cmds[i].kind == CmdKind::LuaFn;
        if (native) {
            cur.idx.push_back(static_cast<int>(i));
        } else {
            flush();
            Segment ext;
            ext.native = false;
            ext.idx.push_back(static_cast<int>(i));
            segs.push_back(ext);
        }
    }
    flush();
    return segs;
}

namespace {

// Optimal string alignment distance: Levenshtein, plus a swapped pair of
// adjacent characters counted as one edit rather than two. Worth the extra
// row, because that is what a mistyped command usually is -- `grpe` is one
// slip from `grep`, and plain Levenshtein rates it no closer than a dozen
// unrelated names two substitutions away.
int edit_distance(const std::string& a, const std::string& b) {
    int m = static_cast<int>(a.size()), n = static_cast<int>(b.size());
    std::vector<int> prev2(n + 1), prev(n + 1), curr(n + 1);
    for (int j = 0; j <= n; ++j) prev[j] = j;
    for (int i = 1; i <= m; ++i) {
        curr[0] = i;
        for (int j = 1; j <= n; ++j) {
            int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            curr[j] = std::min({prev[j] + 1, curr[j - 1] + 1, prev[j - 1] + cost});
            if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1])
                curr[j] = std::min(curr[j], prev2[j - 2] + 1);
        }
        prev2.swap(prev);
        prev.swap(curr);
    }
    return prev[n];
}

bool has_glob_chars(const std::string& s) {
    bool in_bracket = false;
    for (char c : s) {
        if (c == '[') { in_bracket = true; continue; }
        if (c == ']') { in_bracket = false; continue; }
        if (!in_bracket && (c == '*' || c == '?')) return true;
    }
    return false;
}

std::vector<std::string> glob_expand(const std::string& pattern) {
    glob_t results{};
    int flags = GLOB_NOCHECK | GLOB_NOMAGIC;
    if (glob(pattern.c_str(), flags, nullptr, &results) != 0 || results.gl_pathc == 0) {
        globfree(&results);
        return {pattern};
    }
    std::vector<std::string> out;
    out.reserve(results.gl_pathc);
    for (size_t i = 0; i < results.gl_pathc; ++i)
        out.emplace_back(results.gl_pathv[i]);
    globfree(&results);
    return out;
}

std::string find_similar_command(const std::string& name, int threshold) {
    std::string best;
    int best_dist = threshold + 1;
    for (const std::string& candidate : path_executables()) {
        // Length alone bounds the edit distance, and skipping the obvious
        // misses keeps this off the critical path of a typo'd command.
        if (std::labs(static_cast<long>(candidate.size()) - static_cast<long>(name.size())) >= best_dist)
            continue;
        int dist = edit_distance(name, candidate);
        if (dist < best_dist) { best_dist = dist; best = candidate; }
    }
    return best_dist <= threshold ? best : "";
}

// External stdout -> native input default coercion: split into an array of
// line-strings, dropping the trailing empty element from a final newline.
LuaValue drain_fd_to_lines(int fd) {
    std::string buf;
    char chunk[4096];
    ssize_t n;
    while ((n = read(fd, chunk, sizeof chunk)) > 0) buf.append(chunk, static_cast<size_t>(n));
    std::vector<std::string> lines;
    size_t start = 0;
    for (size_t i = 0; i < buf.size(); ++i) {
        if (buf[i] == '\n') { lines.push_back(buf.substr(start, i - start)); start = i + 1; }
    }
    if (start < buf.size()) lines.push_back(buf.substr(start));
    return LuaValue::from_lines(std::move(lines));
}

// Swaps the process's own stdin/stdout/stderr and puts them back afterwards.
// A builtin writes to fd 1 like everything else, so this is what makes
// `echo hi > f` and `jobs 2>/dev/null` work without forking.
class FdSwap {
public:
    FdSwap() = default;
    ~FdSwap() { restore(); }
    FdSwap(const FdSwap&) = delete;
    FdSwap& operator=(const FdSwap&) = delete;

    void to(int target, int fd) {
        if (target < 0 || target > 2) return;
        if (saved_[target] < 0) {
            if (target != STDIN_FILENO) std::fflush(target == STDERR_FILENO ? stderr : stdout);
            saved_[target] = dup(target);
        }
        dup2(fd, target);
    }

    void restore() {
        for (int i = 2; i >= 0; --i) {
            if (saved_[i] < 0) continue;
            if (i != STDIN_FILENO) std::fflush(i == STDERR_FILENO ? stderr : stdout);
            dup2(saved_[i], i);
            close(saved_[i]);
            saved_[i] = -1;
        }
    }

private:
    int saved_[3] = {-1, -1, -1};
};

int open_redirect(RedirKind kind, const std::string& path) {
    int flags = 0;
    switch (kind) {
        case RedirKind::In: flags = O_RDONLY; break;
        case RedirKind::Append: flags = O_WRONLY | O_CREAT | O_APPEND; break;
        case RedirKind::ErrToOut: return -1; // no file involved
        default: flags = O_WRONLY | O_CREAT | O_TRUNC; break;
    }
    int fd = open(path.c_str(), flags, 0644);
    if (fd < 0) std::fprintf(stderr, "wisp: %s: %s\n", path.c_str(), std::strerror(errno));
    return fd;
}

// Wires a command's redirects onto the current process, in written order so
// `> f 2>&1` and `2>&1 > f` differ the way they do in bash. `swap` remembers
// the old fds for an in-process builtin; the forked-child path passes null and
// just execs over the top.
bool apply_redirects(const std::vector<std::pair<RedirKind, std::string>>& rs, FdSwap* swap) {
    auto wire = [&](int target, int fd) {
        if (swap) swap->to(target, fd);
        else dup2(fd, target);
    };
    for (auto& r : rs) {
        if (r.first == RedirKind::ErrToOut) {
            wire(STDERR_FILENO, STDOUT_FILENO);
            continue;
        }
        int fd = open_redirect(r.first, r.second);
        if (fd < 0) return false;
        switch (r.first) {
            case RedirKind::In: wire(STDIN_FILENO, fd); break;
            case RedirKind::ErrOut: wire(STDERR_FILENO, fd); break;
            case RedirKind::OutErr: wire(STDOUT_FILENO, fd); wire(STDERR_FILENO, fd); break;
            default: wire(STDOUT_FILENO, fd); break;
        }
        close(fd);
    }
    return true;
}

// NAME=value, with NAME a shell identifier -- `FOO=bar cmd`, not `a=b` inside
// a longer word or `--opt=x`.
bool is_assignment(const std::string& s) {
    size_t eq = s.find('=');
    if (eq == 0 || eq == std::string::npos) return false;
    if (std::isdigit(static_cast<unsigned char>(s[0]))) return false;
    for (size_t i = 0; i < eq; ++i)
        if (!std::isalnum(static_cast<unsigned char>(s[i])) && s[i] != '_') return false;
    return true;
}

void apply_assignment(const std::string& kv) {
    size_t eq = kv.find('=');
    setenv(kv.substr(0, eq).c_str(), kv.substr(eq + 1).c_str(), 1);
}

std::string describe_pipeline(const Pipeline& pl) {
    std::string out;
    for (size_t i = 0; i < pl.commands.size(); ++i) {
        if (i) out += " | ";
        for (size_t j = 0; j < pl.commands[i].argv.size(); ++j) {
            if (j) out += ' ';
            out += pl.commands[i].argv[j].text;
        }
    }
    return out;
}

} // namespace

Executor::Executor(lua_State* L) : L_(L) {
    lua_env::set_shell_callback([this](const std::string& cmd) { return run_line_str(cmd); });
}

void Executor::init_job_control() {
    shell_terminal_ = STDIN_FILENO;
    interactive_ = isatty(shell_terminal_);
    if (!interactive_) return;

    while (tcgetpgrp(shell_terminal_) != (shell_pgid_ = getpgrp())) kill(-shell_pgid_, SIGTTIN);

    struct sigaction ign {};
    ign.sa_handler = SIG_IGN;
    sigemptyset(&ign.sa_mask);
    sigaction(SIGINT, &ign, nullptr);
    sigaction(SIGQUIT, &ign, nullptr);
    sigaction(SIGTSTP, &ign, nullptr);
    sigaction(SIGTTIN, &ign, nullptr);
    sigaction(SIGTTOU, &ign, nullptr);

    shell_pgid_ = getpid();
    setpgid(shell_pgid_, shell_pgid_);
    tcsetpgrp(shell_terminal_, shell_pgid_);
    tcgetattr(shell_terminal_, &shell_tmodes_);
}

int Executor::run_line_str(const std::string& line) {
    Line parsed;
    try {
        parsed = parse_line(line);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        last_status_ = 2;
        return 2;
    }
    return run_line(parsed);
}

int Executor::run_line(const Line& line) {
    int status = 0;
    for (auto& job_ast : line.jobs) {
        try {
            status = run_job(job_ast);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "%s\n", e.what());
            status = 1;
        }
    }
    last_status_ = status;
    return status;
}

int Executor::run_job(const Job& job_ast) { return run_and_or(job_ast.and_or, job_ast.background); }

int Executor::run_and_or(const AndOr& ao, bool background) {
    int status = 0;
    for (size_t i = 0; i < ao.parts.size(); ++i) {
        const AndOrPart& part = ao.parts[i];
        bool is_last = (i + 1 == ao.parts.size());
        // Backgrounding an &&/|| chain only applies to its last pipeline --
        // earlier parts must run to completion in the foreground anyway,
        // since their exit status decides whether the chain continues.
        status = run_pipeline(part.pipeline, is_last && background, describe_pipeline(part.pipeline));
        if (part.op_after == BoolOp::And && status != 0) break;
        if (part.op_after == BoolOp::Or && status == 0) break;
    }
    return status;
}

std::string Executor::expand_word(const Word& w) {
    if (w.literal()) return w.text; // fully single-quoted: never expanded

    std::string in = w.text, prot = w.prot;
    if (!in.empty() && in[0] == '~' && prot[0] == '0' && (in.size() == 1 || in[1] == '/')) {
        const char* home = std::getenv("HOME");
        if (home) {
            in = home + in.substr(1);
            prot = std::string(std::strlen(home), '0') + prot.substr(1);
        }
    }

    std::string out;
    size_t i = 0;
    while (i < in.size()) {
        char c = in[i];
        // A '$' that was quoted or backslash-escaped is just a dollar sign.
        if (c != '$' || i + 1 >= in.size() || prot[i] == '1') { out += c; ++i; continue; }
        char next = in[i + 1];
        if (next == '?') {
            out += std::to_string(last_status_);
            i += 2;
        } else if (next == '(') {
            size_t close = match_paren(in, i + 1);
            if (close == std::string::npos) { out += c; ++i; continue; }
            out += run_command_substitution(in.substr(i + 2, close - (i + 2)));
            i = close + 1;
        } else if (std::isalpha(static_cast<unsigned char>(next)) || next == '_') {
            size_t j = i + 1;
            while (j < in.size() && (std::isalnum(static_cast<unsigned char>(in[j])) || in[j] == '_')) ++j;
            const char* val = std::getenv(in.substr(i + 1, j - (i + 1)).c_str());
            out += val ? val : "";
            i = j;
        } else if (next == '{' && i + 2 < in.size()) {
            size_t close = in.find('}', i + 2);
            if (close == std::string::npos) { out += c; ++i; continue; }
            std::string inner = in.substr(i + 2, close - i - 2);
            std::string varname, defval;
            size_t dash = inner.find(":-");
            if (dash != std::string::npos) {
                varname = inner.substr(0, dash);
                defval = inner.substr(dash + 2);
            } else {
                varname = inner;
            }
            const char* val = varname.empty() ? nullptr : std::getenv(varname.c_str());
            out += (val && val[0]) ? val : defval;
            i = close + 1;
        } else {
            out += c;
            ++i;
        }
    }
    return out;
}

std::vector<std::string> Executor::expand_words(const Word& w) {
    std::string expanded = expand_word(w);
    if (w.literal()) return {expanded};

    // brace expansion first
    auto braced = brace_expand(expanded);

    // then glob expansion on each result
    std::vector<std::string> result;
    for (auto& s : braced) {
        if (has_glob_chars(s)) {
            auto matches = glob_expand(s);
            result.insert(result.end(), matches.begin(), matches.end());
        } else {
            result.push_back(s);
        }
    }
    return result;
}

std::string Executor::run_command_substitution(const std::string& inner) {
    int pipefd[2];
    if (pipe(pipefd) != 0) return "";
    std::fflush(nullptr); // else the child inherits a copy of our pending output
    pid_t pid = fork();
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGTSTP, SIG_DFL);
        signal(SIGTTIN, SIG_DFL);
        signal(SIGTTOU, SIG_DFL);
        // A subshell does no job control. Left interactive, an external
        // command in here would tcsetpgrp() the terminal over to its own
        // process group, and nothing would ever hand it back -- the real
        // shell would then read from stdin while in a background group and
        // get EIO, which looks exactly like EOF and quietly ends the session.
        interactive_ = false;
        jobs_.clear();
        int rc = 0;
        try {
            rc = run_line(parse_line(inner));
        } catch (const std::exception& e) {
            std::fprintf(stderr, "%s\n", e.what());
            rc = 2;
        }
        // Builtins write through stdio, and stdout is a fully-buffered pipe
        // here -- _exit() would drop everything they produced.
        std::fflush(nullptr);
        _exit(rc);
    }
    close(pipefd[1]);
    std::string out;
    char buf[4096];
    ssize_t n;
    while ((n = read(pipefd[0], buf, sizeof buf)) > 0) out.append(buf, static_cast<size_t>(n));
    close(pipefd[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    while (!out.empty() && out.back() == '\n') out.pop_back();
    return out;
}

std::vector<ExpCmd> Executor::expand_and_classify(const Pipeline& pl) {
    std::vector<ExpCmd> cmds;
    cmds.reserve(pl.commands.size());
    for (auto& c : pl.commands) {
        ExpCmd ec;
        ec.argv.reserve(c.argv.size());
        for (auto& w : c.argv) {
            auto expanded = expand_words(w);
            ec.argv.insert(ec.argv.end(), expanded.begin(), expanded.end());
        }
        ec.redirects = c.redirects;

        // Leading NAME=value words set the environment rather than naming the
        // command. With nothing left after them the assignment *is* the
        // command, which is exactly what `export` already does.
        size_t lead = 0;
        while (lead < ec.argv.size() && is_assignment(ec.argv[lead])) ++lead;
        if (lead > 0) {
            ec.assignments.assign(ec.argv.begin(), ec.argv.begin() + static_cast<long>(lead));
            ec.argv.erase(ec.argv.begin(), ec.argv.begin() + static_cast<long>(lead));
            if (ec.argv.empty()) {
                ec.argv.push_back("export");
                ec.argv.insert(ec.argv.end(), ec.assignments.begin(), ec.assignments.end());
                ec.assignments.clear();
            }
        }

        if (!ec.argv.empty() && ec.argv[0] == "command" && ec.argv.size() > 1) {
            // Escape hatch past a same-named Lua function: force external.
            ec.argv.erase(ec.argv.begin());
            ec.kind = CmdKind::External;
        } else if (!ec.argv.empty()) {
            ec.kind = lua_env::resolve_command(L_, ec.argv[0]);
        }
        cmds.push_back(std::move(ec));
    }
    return cmds;
}

LuaValue Executor::run_native_run(const std::vector<ExpCmd>& cmds, const std::vector<int>& idx,
                                   const LuaValue& input, bool has_input) {
    lua_env::InterruptGuard guard(L_);

    // The value flowing through the run lives on the Lua stack, not in a
    // LuaValue: a round trip through C++ between every pair of stages would
    // deep-copy every record twice per stage, which is precisely the cost
    // structured pipelines exist to avoid. It crosses the boundary once on
    // the way in and once on the way out.
    const int base = lua_gettop(L_);
    struct StackReset {
        lua_State* L;
        int base;
        ~StackReset() { lua_settop(L, base); }
    } reset{L_, base};

    if (has_input) lua_env::push_value(L_, input);

    // When this run ends the pipeline there is no downstream stage, so the
    // final value never has to be materialised in C++ at all.
    const bool tail_of_pipeline = !idx.empty() && static_cast<size_t>(idx.back()) + 1 == cmds.size();

    for (int i : idx) {
        const ExpCmd& ec = cmds[i];
        bool last_in_pipeline = (static_cast<size_t>(i) + 1 == cmds.size());
        for (auto& kv : ec.assignments) apply_assignment(kv);

        std::vector<std::pair<RedirKind, std::string>> redirs;
        for (auto& r : ec.redirects) redirs.emplace_back(r.kind, expand_word(r.target));

        // A builtin feeding a later stage has to hand over its bytes rather
        // than print them. Temp file, not an OS pipe: nobody is draining the
        // other end yet, so anything over 64K would deadlock.
        FILE* capture = (!last_in_pipeline && ec.kind == CmdKind::Builtin) ? std::tmpfile() : nullptr;

        FdSwap swap;
        if (capture) swap.to(STDOUT_FILENO, fileno(capture));
        if (!apply_redirects(redirs, &swap)) {
            last_status_ = 1;
            if (capture) std::fclose(capture);
            continue;
        }

        if (ec.kind == CmdKind::Builtin) {
            last_status_ = run_builtin(ec.argv);
        } else {
            std::vector<std::string> args(ec.argv.begin() + 1, ec.argv.end());
            lua_env::call_lua_function_top(L_, ec.argv[0], args, has_input);
            has_input = true;
            last_status_ = (lua_isnil(L_, -1) || (lua_isboolean(L_, -1) && !lua_toboolean(L_, -1))) ? 1 : 0;
            // Rendered here, inside the redirect, so `f > out` catches the
            // returned value and anything the function printed alike.
            if (last_in_pipeline) {
                std::string rendered = lua_env::render_value(lua_env::pull_value(L_, -1));
                std::fwrite(rendered.data(), 1, rendered.size(), stdout);
            }
        }

        std::fflush(stdout);
        swap.restore();

        if (capture) {
            std::rewind(capture);
            lua_settop(L_, base); // the builtin's bytes replace whatever was flowing
            lua_env::push_value(L_, drain_fd_to_lines(fileno(capture)));
            has_input = true;
            std::fclose(capture);
        }
    }
    return (has_input && !tail_of_pipeline) ? lua_env::pull_value(L_, -1) : LuaValue::nil();
}

int Executor::run_builtin(const std::vector<std::string>& argv) {
    const std::string& name = argv[0];

    if (name == "cd") {
        std::string target;
        if (argv.size() > 1) target = argv[1];
        else { const char* home = std::getenv("HOME"); target = home ? home : "/"; }
        if (target == "-") {
            if (prev_dir_.empty()) {
                if (const char* old = std::getenv("OLDPWD")) prev_dir_ = old;
            }
            if (prev_dir_.empty()) {
                std::fprintf(stderr, "wisp: cd: OLDPWD not set\n");
                return 1;
            }
            target = prev_dir_;
        }
        char cwd[4096];
        std::string from = getcwd(cwd, sizeof cwd) ? cwd : "";
        if (chdir(target.c_str()) != 0) {
            std::fprintf(stderr, "wisp: cd: %s: %s\n", target.c_str(), std::strerror(errno));
            return 1;
        }
        prev_dir_ = from;
        // Keep $PWD/$OLDPWD honest -- a prompt() reading them, or any child
        // process, otherwise sees whatever directory the shell started in.
        if (!from.empty()) setenv("OLDPWD", from.c_str(), 1);
        if (getcwd(cwd, sizeof cwd)) setenv("PWD", cwd, 1);
        return 0;
    }
    if (name == "exit") {
        std::exit(argv.size() > 1 ? std::atoi(argv[1].c_str()) : last_status_);
    }
    if (name == "export") {
        for (size_t i = 1; i < argv.size(); ++i) {
            size_t eq = argv[i].find('=');
            if (eq == std::string::npos) continue;
            setenv(argv[i].substr(0, eq).c_str(), argv[i].substr(eq + 1).c_str(), 1);
        }
        return 0;
    }
    if (name == "jobs") {
        bool show_pids = argv.size() > 1 && argv[1] == "-l";
        for (auto& j : jobs_) {
            if (show_pids) {
                const char* state = j.all_stopped() ? "Stopped" : (j.all_completed() ? "Done" : "Running");
                pid_t pid = j.procs.empty() ? -1 : j.procs[0].pid;
                std::printf("[%d]  %d %s    %s\n", j.id, pid, state, j.cmdline.c_str());
            } else {
                std::printf("%s\n", format_job_line(j).c_str());
            }
        }
        return 0;
    }
    if (name == "fg" || name == "bg") {
        ShellJob* j = nullptr;
        if (argv.size() > 1) {
            std::string spec = argv[1];
            if (!spec.empty() && spec[0] == '%') spec = spec.substr(1);
            j = find_job(std::atoi(spec.c_str()));
        } else if (!jobs_.empty()) {
            j = &jobs_.back();
        }
        if (!j) {
            std::fprintf(stderr, "wisp: %s: no such job\n", name.c_str());
            return 1;
        }
        killpg(j->pgid, SIGCONT);
        for (auto& p : j->procs) p.stopped = false;
        if (name == "fg") {
            put_job_in_foreground(*j);
        } else {
            j->background = true;
            std::fprintf(stderr, "[%d] %d\n", j->id, j->procs.empty() ? -1 : j->procs.back().pid);
        }
        return 0;
    }
    if (name == "command") return 0; // bare "command" with nothing to run: no-op

    if (name == "kill") {
        if (argv.size() < 2) {
            std::fprintf(stderr, "wisp: kill: usage: kill [-signal] %%pid|%%job ...\n");
            return 1;
        }
        int sig = SIGTERM;
        size_t start = 1;
        if (argv.size() > 2 && argv[1].size() > 1 && argv[1][0] == '-') {
            sig = std::atoi(argv[1].c_str() + 1);
            if (sig <= 0) sig = SIGTERM;
            start = 2;
        }
        int rc = 0;
        for (size_t i = start; i < argv.size(); ++i) {
            const std::string& spec = argv[i];
            if (!spec.empty() && spec[0] == '%') {
                int id = std::atoi(spec.c_str() + 1);
                ShellJob* j = find_job(id);
                if (!j) {
                    std::fprintf(stderr, "wisp: kill: %s: no such job\n", spec.c_str());
                    rc = 1;
                    continue;
                }
                if (killpg(j->pgid, sig) != 0) {
                    std::fprintf(stderr, "wisp: kill: %s: %s\n", spec.c_str(), std::strerror(errno));
                    rc = 1;
                } else if (j->all_stopped() && sig != SIGKILL && sig != SIGSTOP && sig != SIGCONT) {
                    // A stopped process never gets to act on the signal it was
                    // just sent; nudge it back onto the CPU so it can die.
                    killpg(j->pgid, SIGCONT);
                    for (auto& p : j->procs) p.stopped = false;
                }
            } else {
                pid_t pid = static_cast<pid_t>(std::atoi(spec.c_str()));
                if (pid <= 0) {
                    std::fprintf(stderr, "wisp: kill: %s: invalid process id\n", spec.c_str());
                    rc = 1;
                    continue;
                }
                if (kill(pid, sig) != 0) {
                    std::fprintf(stderr, "wisp: kill: %s: %s\n", spec.c_str(), std::strerror(errno));
                    rc = 1;
                }
            }
        }
        return rc;
    }

    if (name == "echo") {
        bool newline = true;
        bool process_escape = false;
        size_t start = 1;
        if (start < argv.size() && argv[start] == "-n") {
            newline = false;
            ++start;
        }
        if (start < argv.size() && argv[start] == "-e") {
            process_escape = true;
            ++start;
        }
        for (size_t i = start; i < argv.size(); ++i) {
            if (i > start) std::fputc(' ', stdout);
            if (process_escape) {
                for (size_t j = 0; j < argv[i].size(); ++j) {
                    char c = argv[i][j];
                    if (c == '\\' && j + 1 < argv[i].size()) {
                        char next = argv[i][j + 1];
                        if (next == 'n') { std::fputc('\n', stdout); ++j; continue; }
                        if (next == 't') { std::fputc('\t', stdout); ++j; continue; }
                        if (next == '\\') { std::fputc('\\', stdout); ++j; continue; }
                    }
                    std::fputc(c, stdout);
                }
            } else {
                std::fputs(argv[i].c_str(), stdout);
            }
        }
        if (newline) std::fputc('\n', stdout);
        return 0;
    }

    if (name == "source") {
        if (argv.size() < 2) {
            std::fprintf(stderr, "wisp: source: usage: source <file.lua>\n");
            return 1;
        }
        if (luaL_loadfile(L_, argv[1].c_str()) != LUA_OK) {
            std::fprintf(stderr, "wisp: source: %s: %s\n", argv[1].c_str(), lua_tostring(L_, -1));
            lua_pop(L_, 1);
            return 1;
        }
        if (lua_pcall(L_, 0, LUA_MULTRET, 0) != LUA_OK) {
            std::fprintf(stderr, "wisp: source: %s\n", lua_tostring(L_, -1));
            lua_pop(L_, 1);
            return 1;
        }
        return 0;
    }

    if (name == "disown") {
        if (argv.size() < 2) {
            std::fprintf(stderr, "wisp: disown: usage: disown <%%job>\n");
            return 1;
        }
        std::string spec = argv[1];
        if (!spec.empty() && spec[0] == '%') spec = spec.substr(1);
        int id = std::atoi(spec.c_str());
        for (size_t i = 0; i < jobs_.size(); ++i) {
            if (jobs_[i].id == id) {
                jobs_.erase(jobs_.begin() + static_cast<long>(i));
                return 0;
            }
        }
        std::fprintf(stderr, "wisp: disown: %s: no such job\n", argv[1].c_str());
        return 1;
    }

    if (name == "wait") {
        if (argv.size() < 2) {
            // wait for all background jobs
            for (auto& j : jobs_) {
                if (!j.all_completed()) wait_for_job(j);
            }
            return 0;
        }
        std::string spec = argv[1];
        if (!spec.empty() && spec[0] == '%') spec = spec.substr(1);
        int id = std::atoi(spec.c_str());
        ShellJob* j = find_job(id);
        if (!j) {
            std::fprintf(stderr, "wisp: wait: %s: no such job\n", argv[1].c_str());
            return 1;
        }
        wait_for_job(*j);
        return 0;
    }

    if (name == "pwd") {
        char cwd[4096];
        if (getcwd(cwd, sizeof cwd)) {
            std::fputs(cwd, stdout);
            std::fputc('\n', stdout);
            return 0;
        }
        std::fprintf(stderr, "wisp: pwd: %s\n", std::strerror(errno));
        return 1;
    }

    if (name == "type") {
        if (argv.size() < 2) {
            std::fprintf(stderr, "wisp: type: usage: type <command>\n");
            return 1;
        }
        const std::string& cmd = argv[1];
        switch (lua_env::resolve_command(L_, cmd)) {
            case CmdKind::Builtin: std::printf("%s is a shell builtin\n", cmd.c_str()); return 0;
            case CmdKind::LuaFn: std::printf("%s is a Lua function\n", cmd.c_str()); return 0;
            case CmdKind::External: break;
        }
        if (std::string full = which(cmd); !full.empty()) {
            std::printf("%s is %s\n", cmd.c_str(), full.c_str());
            return 0;
        }
        std::fprintf(stderr, "wisp: type: %s: not found\n", cmd.c_str());
        return 1;
    }

    if (name == "pkg") return run_pkg(argv);

    return 1; // unreachable: is_builtin_name() gates entry to this function
}

// `pkg` is a spelling of luarocks, not a package manager of its own: rocks
// land in the tree lua_env.cpp already puts on package.path, so `require()`
// picks them up. Forked rather than system()'d so a package name can't reach
// /bin/sh as syntax.
int Executor::run_pkg(const std::vector<std::string>& argv) {
    static const std::pair<const char*, const char*> kSubcmds[] = {
        {"install", "install"}, {"remove", "remove"}, {"list", "list"},
        {"search", "search"},   {"info", "show"},     {"update", "install"},
    };

    if (argv.size() < 2) {
        std::fprintf(stderr, "usage: pkg install|remove|update <rock> | list | search <query> | info <rock>\n");
        return 1;
    }
    const char* mapped = nullptr;
    for (auto& s : kSubcmds)
        if (argv[1] == s.first) mapped = s.second;
    if (!mapped) {
        std::fprintf(stderr, "wisp: pkg: %s: unknown command\n", argv[1].c_str());
        return 1;
    }
    if (which("luarocks").empty()) {
        std::fprintf(stderr, "wisp: pkg: luarocks not found on $PATH (install your distro's luarocks package)\n");
        return 1;
    }

    // Pinned to the Lua we linked against: a luarocks defaulting to another
    // version would install somewhere require() is never going to look.
    std::vector<std::string> cmd = {"luarocks", "--local", "--lua-version=" LUA_VDIR, mapped};
    cmd.insert(cmd.end(), argv.begin() + 2, argv.end());

    std::fflush(nullptr);
    pid_t pid = fork();
    if (pid < 0) return 1;
    if (pid == 0) {
        std::vector<char*> c_argv;
        for (auto& a : cmd) c_argv.push_back(const_cast<char*>(a.c_str()));
        c_argv.push_back(nullptr);
        execvp(c_argv[0], c_argv.data());
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

ShellJob* Executor::find_job(int id) {
    for (auto& j : jobs_)
        if (j.id == id) return &j;
    return nullptr;
}

void Executor::mark_process_status(pid_t pid, int status) {
    for (auto& j : jobs_) {
        for (auto& p : j.procs) {
            if (p.pid == pid) {
                p.status = status;
                if (WIFSTOPPED(status)) p.stopped = true;
                else { p.completed = true; p.stopped = false; }
                return;
            }
        }
    }
}

void Executor::wait_for_job(ShellJob& job) {
    for (;;) {
        bool any_running = false;
        for (auto& p : job.procs)
            if (!p.completed && !p.stopped) any_running = true;
        if (!any_running) break;

        int status = 0;
        pid_t pid = waitpid(-job.pgid, &status, WUNTRACED);
        if (pid < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (auto& p : job.procs) {
            if (p.pid == pid) {
                p.status = status;
                if (WIFSTOPPED(status)) p.stopped = true;
                else { p.completed = true; p.stopped = false; }
                break;
            }
        }
    }
}

void Executor::put_job_in_foreground(ShellJob& job) {
    if (interactive_) tcsetpgrp(shell_terminal_, job.pgid);
    wait_for_job(job);
    if (interactive_) {
        tcsetpgrp(shell_terminal_, shell_pgid_);
        // Restore the shell's own baseline termios *before* returning to the
        // prompt loop's next linenoise() call -- linenoise otherwise adopts
        // whatever raw/cooked state is current, which may be a child's
        // leftover mode rather than the shell's own.
        tcsetattr(shell_terminal_, TCSADRAIN, &shell_tmodes_);
    }
    if (job.all_stopped()) {
        std::fprintf(stderr, "\n[%d]+ Stopped    %s\n", job.id, job.cmdline.c_str());
        if (!find_job(job.id)) jobs_.push_back(job);
    } else {
        for (size_t i = 0; i < jobs_.size(); ++i) {
            if (jobs_[i].id == job.id) {
                jobs_.erase(jobs_.begin() + static_cast<long>(i));
                break;
            }
        }
    }
}

void Executor::reap_background() {
    int status = 0;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED | WCONTINUED)) > 0) mark_process_status(pid, status);

    for (size_t i = 0; i < jobs_.size();) {
        if (jobs_[i].all_completed()) {
            std::fprintf(stderr, "[%d]+ Done    %s\n", jobs_[i].id, jobs_[i].cmdline.c_str());
            jobs_.erase(jobs_.begin() + static_cast<long>(i));
        } else {
            ++i;
        }
    }
}

int Executor::run_pipeline(const Pipeline& pl, bool background, const std::string& cmdline_text) {
    std::vector<ExpCmd> cmds = expand_and_classify(pl);
    std::vector<Segment> segs = build_segments(cmds);

    bool any_external = false;
    for (auto& s : segs)
        if (!s.native) any_external = true;

    if (background && !any_external) {
        std::fprintf(stderr, "wisp: no external process to background, running in foreground\n");
        background = false;
    }

    ShellJob job;
    bool have_job = any_external;
    if (have_job) {
        job.id = next_job_id_++;
        job.cmdline = cmdline_text;
        job.background = background;
    }

    struct Carry {
        enum class Kind { None, Value, Fd } kind = Kind::None;
        LuaValue value;
        int fd = -1;
    } carry;

    for (size_t si = 0; si < segs.size(); ++si) {
        bool is_last = (si + 1 == segs.size());
        Segment& seg = segs[si];

        if (seg.native) {
            LuaValue input;
            bool has_input = false;
            if (carry.kind == Carry::Kind::Fd) {
                input = drain_fd_to_lines(carry.fd);
                close(carry.fd);
                has_input = true;
            } else if (carry.kind == Carry::Kind::Value) {
                input = carry.value;
                has_input = true;
            }

            // Display of a final value happens inside run_native_run, where
            // the command's own redirects are still in force.
            LuaValue out = run_native_run(cmds, seg.idx, input, has_input);
            if (!is_last) {
                carry = Carry{};
                carry.kind = Carry::Kind::Value;
                carry.value = std::move(out);
            }
            continue;
        }

        // External stage: forks, exactly as a traditional shell would.
        const ExpCmd& ec = cmds[seg.idx[0]];

        int in_fd = -1;
        int writer_fd = -1;
        bool spawn_writer = false;
        std::string serialized;

        if (carry.kind == Carry::Kind::Fd) {
            in_fd = carry.fd;
        } else if (carry.kind == Carry::Kind::Value) {
            serialized = lua_env::render_value(carry.value);
            int pfd[2];
            pipe(pfd);
            in_fd = pfd[0];
            writer_fd = pfd[1];
            spawn_writer = true;
        }

        int out_pipe[2] = {-1, -1};
        if (!is_last) pipe(out_pipe);

        // Expanded before the fork: a $(...) in a redirect target runs its own
        // subshell, and that belongs to us, not to the child.
        std::vector<std::pair<RedirKind, std::string>> redirs;
        for (auto& r : ec.redirects) redirs.emplace_back(r.kind, expand_word(r.target));

        std::fflush(nullptr);
        pid_t pid = fork();
        if (pid == 0) {
            pid_t pg = (job.pgid == -1) ? 0 : job.pgid;
            if (have_job) setpgid(0, pg);
            if (interactive_ && !background && have_job) tcsetpgrp(shell_terminal_, getpgrp());

            signal(SIGINT, SIG_DFL);
            signal(SIGQUIT, SIG_DFL);
            signal(SIGTSTP, SIG_DFL);
            signal(SIGTTIN, SIG_DFL);
            signal(SIGTTOU, SIG_DFL);

            if (in_fd != -1) dup2(in_fd, STDIN_FILENO);
            if (!is_last) dup2(out_pipe[1], STDOUT_FILENO);
            if (spawn_writer) close(writer_fd);
            if (in_fd != -1) close(in_fd);
            if (!is_last) { close(out_pipe[0]); close(out_pipe[1]); }

            if (!apply_redirects(redirs, nullptr)) _exit(1);
            for (auto& kv : ec.assignments) apply_assignment(kv);

            std::vector<char*> argv_c;
            argv_c.reserve(ec.argv.size() + 1);
            for (auto& a : ec.argv) argv_c.push_back(const_cast<char*>(a.c_str()));
            argv_c.push_back(nullptr);
            execvp(argv_c[0], argv_c.data());
            if (errno == ENOENT) {
                std::fprintf(stderr, "wisp: %s: command not found", ec.argv[0].c_str());
                std::string hint = find_similar_command(ec.argv[0], 3);
                if (!hint.empty()) std::fprintf(stderr, " (did you mean '%s'?)", hint.c_str());
                std::fputc('\n', stderr);
            } else {
                std::fprintf(stderr, "wisp: %s: %s\n", ec.argv[0].c_str(), std::strerror(errno));
            }
            _exit(127);
        }

        // Parent.
        if (have_job) {
            if (job.pgid == -1) job.pgid = pid;
            setpgid(pid, job.pgid);
            if (interactive_ && !background) tcsetpgrp(shell_terminal_, job.pgid);
            Process proc;
            proc.pid = pid;
            proc.argv = ec.argv;
            job.procs.push_back(std::move(proc));
        }

        if (spawn_writer) {
            close(in_fd); // the shell only feeds the child, never reads its own pipe back
            std::thread([wfd = writer_fd, data = std::move(serialized)]() mutable {
                size_t off = 0;
                while (off < data.size()) {
                    ssize_t n = write(wfd, data.data() + off, data.size() - off);
                    if (n <= 0) break; // EPIPE or child exited early: stop writing
                    off += static_cast<size_t>(n);
                }
                close(wfd);
            }).detach(); // pure bytes, no Lua state touched -- safe to fire-and-forget
        } else if (in_fd != -1) {
            close(in_fd);
        }

        if (!is_last) {
            close(out_pipe[1]);
            carry = Carry{};
            carry.kind = Carry::Kind::Fd;
            carry.fd = out_pipe[0];
        } else {
            carry = Carry{};
        }
    }

    if (have_job) {
        if (background) {
            std::fprintf(stderr, "[%d] %d\n", job.id, job.procs.empty() ? -1 : job.procs.back().pid);
            jobs_.push_back(std::move(job));
        } else {
            put_job_in_foreground(job);
            int st = job.procs.empty() ? 0 : job.procs.back().status;
            if (WIFEXITED(st)) last_status_ = WEXITSTATUS(st);
            else if (WIFSIGNALED(st)) last_status_ = 128 + WTERMSIG(st);
        }
    }

    return last_status_;
}
