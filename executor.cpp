#include "executor.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <exception>
#include <fcntl.h>
#include <glob.h>
#include <sys/stat.h>
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

// Ctrl-C during an in-process native run: reuses lua.c's own standalone-
// interpreter technique (SIGINT handler arms a debug hook; the hook unwinds
// via luaL_error on the next VM instruction) rather than inventing a new
// mechanism. Scoped to just the native-run's execution window via RAII --
// installing it permanently would tax every VM instruction for no benefit
// the rest of the time.
lua_State* g_interrupt_L = nullptr;

int levenshtein(const std::string& a, const std::string& b) {
    int m = static_cast<int>(a.size()), n = static_cast<int>(b.size());
    std::vector<int> prev(n + 1), curr(n + 1);
    for (int j = 0; j <= n; ++j) prev[j] = j;
    for (int i = 1; i <= m; ++i) {
        curr[0] = i;
        for (int j = 1; j <= n; ++j) {
            int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            curr[j] = std::min({prev[j] + 1, curr[j - 1] + 1, prev[j - 1] + cost});
        }
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
    const char* path_env = std::getenv("PATH");
    if (!path_env) return "";
    std::string path(path_env);
    std::string best;
    int best_dist = threshold + 1;
    size_t start = 0;
    while (start <= path.size()) {
        size_t colon = path.find(':', start);
        std::string dir = path.substr(start, colon == std::string::npos ? std::string::npos : colon - start);
        if (dir.empty()) dir = ".";
        DIR* d = opendir(dir.c_str());
        if (d) {
            struct dirent* ent;
            while ((ent = readdir(d)) != nullptr) {
                std::string candidate = ent->d_name;
                if (candidate == "." || candidate == "..") continue;
                std::string full = dir + "/" + candidate;
                struct stat st{};
                if (stat(full.c_str(), &st) != 0 || !(st.st_mode & S_IXUSR)) continue;
                int dist = levenshtein(name, candidate);
                if (dist < best_dist) { best_dist = dist; best = candidate; }
            }
            closedir(d);
        }
        if (colon == std::string::npos) break;
        start = colon + 1;
    }
    return best_dist <= threshold ? best : "";
}

void wisp_sigint_hook(lua_State* L, lua_Debug*) {
    lua_sethook(L, nullptr, 0, 0);
    luaL_error(L, "interrupted");
}

void wisp_sigint_action(int sig) {
    std::signal(sig, SIG_DFL); // a second Ctrl-C during unwind kills the process, same as lua.c
    if (g_interrupt_L) {
        lua_sethook(g_interrupt_L, wisp_sigint_hook,
                    LUA_MASKCALL | LUA_MASKRET | LUA_MASKLINE | LUA_MASKCOUNT, 1);
    }
}

struct SigintGuard {
    struct sigaction old_ {};
    explicit SigintGuard(lua_State* L) {
        g_interrupt_L = L;
        struct sigaction sa {};
        sa.sa_handler = wisp_sigint_action;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGINT, &sa, &old_);
    }
    ~SigintGuard() {
        sigaction(SIGINT, &old_, nullptr);
        g_interrupt_L = nullptr;
    }
};

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
    if (w.literal) return w.text; // single-quoted: never expanded

    std::string in = w.text;
    if (!in.empty() && in[0] == '~' && (in.size() == 1 || in[1] == '/')) {
        const char* home = std::getenv("HOME");
        if (home) in = std::string(home) + in.substr(1);
    }

    std::string out;
    size_t i = 0;
    while (i < in.size()) {
        char c = in[i];
        if (c != '$' || i + 1 >= in.size()) { out += c; ++i; continue; }
        char next = in[i + 1];
        if (next == '?') {
            out += std::to_string(last_status_);
            i += 2;
        } else if (next == '(') {
            size_t close = in.find(')', i + 2);
            if (close == std::string::npos) { out += c; ++i; continue; }
            out += run_command_substitution(in.substr(i + 2, close - (i + 2)));
            i = close + 1;
        } else if (std::isalpha(static_cast<unsigned char>(next)) || next == '_') {
            size_t j = i + 1;
            while (j < in.size() && (std::isalnum(static_cast<unsigned char>(in[j])) || in[j] == '_')) ++j;
            const char* val = std::getenv(in.substr(i + 1, j - (i + 1)).c_str());
            out += val ? val : "";
            i = j;
        } else {
            out += c;
            ++i;
        }
    }
    return out;
}

std::vector<std::string> Executor::expand_words(const Word& w) {
    std::string expanded = expand_word(w);
    if (w.literal) return {expanded};
    if (has_glob_chars(expanded)) {
        auto matches = glob_expand(expanded);
        if (!matches.empty()) return matches;
    }
    return {expanded};
}

std::string Executor::run_command_substitution(const std::string& inner) {
    int pipefd[2];
    if (pipe(pipefd) != 0) return "";
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
        int rc = 0;
        try {
            rc = run_line(parse_line(inner));
        } catch (const std::exception& e) {
            std::fprintf(stderr, "%s\n", e.what());
            rc = 2;
        }
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
                                   LuaValue input, bool has_input) {
    SigintGuard guard(L_);
    LuaValue v = std::move(input);
    bool last_was_luafn = false;
    for (int i : idx) {
        const ExpCmd& ec = cmds[i];
        if (ec.kind == CmdKind::Builtin) {
            last_status_ = run_builtin(ec.argv);
            last_was_luafn = false;
        } else {
            std::vector<std::string> args(ec.argv.begin() + 1, ec.argv.end());
            v = lua_env::call_lua_function(L_, ec.argv[0], args, v, has_input);
            has_input = true;
            last_was_luafn = true;
        }
    }
    if (last_was_luafn) {
        bool falsy = v.type == LuaValue::Type::Nil || (v.type == LuaValue::Type::Bool && !v.b);
        last_status_ = falsy ? 1 : 0;
    }
    return v;
}

int Executor::run_builtin(const std::vector<std::string>& argv) {
    const std::string& name = argv[0];

    if (name == "cd") {
        std::string target;
        if (argv.size() > 1) target = argv[1];
        else { const char* home = std::getenv("HOME"); target = home ? home : "/"; }
        if (chdir(target.c_str()) != 0) {
            std::fprintf(stderr, "wisp: cd: %s: %s\n", target.c_str(), std::strerror(errno));
            return 1;
        }
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
        for (auto& j : jobs_) std::printf("%s\n", format_job_line(j).c_str());
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

    return 1; // unreachable: is_builtin_name() gates entry to this function
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

            LuaValue out = run_native_run(cmds, seg.idx, input, has_input);

            const ExpCmd& last_cmd = cmds[seg.idx.back()];
            // A run ending in a plain builtin (cd/export/jobs/...) has
            // nothing meaningful to show -- `out` is just the untouched
            // nil/passthrough value, not real user data. Only a LuaFn as
            // the final stage produces something worth rendering.
            if (is_last && last_cmd.kind == CmdKind::LuaFn) {
                std::string rendered = lua_env::render_value(out);
                bool wrote_to_file = false;
                for (auto& r : last_cmd.redirects) {
                    if (r.kind == RedirKind::Out || r.kind == RedirKind::Append) {
                        std::string path = expand_word(r.target);
                        FILE* f = std::fopen(path.c_str(), r.kind == RedirKind::Append ? "a" : "w");
                        if (f) {
                            std::fwrite(rendered.data(), 1, rendered.size(), f);
                            std::fclose(f);
                        }
                        wrote_to_file = true;
                    }
                }
                if (!wrote_to_file) std::fwrite(rendered.data(), 1, rendered.size(), stdout);
            } else if (!is_last) {
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

            for (auto& r : ec.redirects) {
                std::string path = expand_word(r.target);
                int flags = 0, fd_target = -1;
                switch (r.kind) {
                    case RedirKind::In: flags = O_RDONLY; fd_target = STDIN_FILENO; break;
                    case RedirKind::Out: flags = O_WRONLY | O_CREAT | O_TRUNC; fd_target = STDOUT_FILENO; break;
                    case RedirKind::Append: flags = O_WRONLY | O_CREAT | O_APPEND; fd_target = STDOUT_FILENO; break;
                    case RedirKind::ErrOut: flags = O_WRONLY | O_CREAT | O_TRUNC; fd_target = STDERR_FILENO; break;
                }
                int fd = open(path.c_str(), flags, 0644);
                if (fd < 0) {
                    std::fprintf(stderr, "wisp: %s: %s\n", path.c_str(), std::strerror(errno));
                    _exit(1);
                }
                dup2(fd, fd_target);
                close(fd);
            }

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
