#pragma once

#include <string>
#include <sys/types.h>
#include <termios.h>
#include <vector>

#include "lua_env.h"
#include "parser.h"

struct Process {
    pid_t pid = -1;
    std::vector<std::string> argv;
    bool completed = false;
    bool stopped = false;
    int status = 0;
};

// Named ShellJob (not Job) because parser.h already uses `Job` for the AST
// node (a pipeline chain plus its background flag) -- same word, different
// layer, so it needs a different name here.
struct ShellJob {
    int id = 0;
    pid_t pgid = -1;
    std::vector<Process> procs;
    std::string cmdline;
    bool background = false;
    struct termios tmodes {};
    bool have_tmodes = false;

    bool all_completed() const;
    bool all_stopped() const;
};

std::string format_job_line(const ShellJob& j);

// A pipeline command after $VAR/$(cmd) expansion, tagged with how it will be
// dispatched. Pure data -- no I/O, no Lua state -- so the native/external
// grouping below is testable without forking anything.
struct ExpCmd {
    std::vector<std::string> argv;
    std::vector<Redirect> redirects;
    CmdKind kind = CmdKind::External;
};

struct Segment {
    bool native = false;   // Builtin/LuaFn run in-process, chaining Lua values
    std::vector<int> idx;  // indices into the pipeline's ExpCmd vector
};

// Consecutive Builtin/LuaFn commands become one native segment (no fork, no
// OS pipe between them); anything else is its own external (forked) segment.
std::vector<Segment> build_segments(const std::vector<ExpCmd>& cmds);

class Executor {
public:
    explicit Executor(lua_State* L);

    // Sets up process-group/terminal control if stdin is a tty. Job control
    // (Ctrl-Z, fg/bg, terminal handoff) degrades to "always foreground,
    // no-op" when not interactive (e.g. piped-in script input).
    void init_job_control();

    // Parses `line` and runs it. Parse errors and Lua/runtime errors are
    // caught here, printed to stderr, and turned into a nonzero status
    // rather than propagating -- this is the one entry point both the main
    // REPL loop and the Lua `sh()` binding call.
    int run_line_str(const std::string& line);

    // Non-blocking sweep for completed/stopped background jobs; call once
    // per prompt iteration so `[1]+ Done ...` notices appear promptly.
    void reap_background();

    int last_status() const { return last_status_; }

private:
    int run_line(const Line& line);
    int run_job(const Job& job_ast);
    int run_and_or(const AndOr& ao, bool background);
    int run_pipeline(const Pipeline& pl, bool background, const std::string& cmdline_text);

    std::string expand_word(const Word& w);
    std::vector<std::string> expand_words(const Word& w);
    std::string run_command_substitution(const std::string& inner);

    std::vector<ExpCmd> expand_and_classify(const Pipeline& pl);
    LuaValue run_native_run(const std::vector<ExpCmd>& cmds, const std::vector<int>& idx,
                             LuaValue input, bool has_input);
    int run_builtin(const std::vector<std::string>& argv);

    void put_job_in_foreground(ShellJob& job);
    void wait_for_job(ShellJob& job);
    void mark_process_status(pid_t pid, int status);
    ShellJob* find_job(int id);

    lua_State* L_;
    bool interactive_ = false;
    pid_t shell_pgid_ = 0;
    int shell_terminal_ = 0;
    struct termios shell_tmodes_ {};

    std::vector<ShellJob> jobs_;
    int next_job_id_ = 1;
    int last_status_ = 0;
};
