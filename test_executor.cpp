#include <cassert>
#include <cstdio>

#include "executor.h"

static ExpCmd mk(CmdKind k) {
    ExpCmd e;
    e.kind = k;
    return e;
}

static void test_single_native() {
    auto segs = build_segments({mk(CmdKind::LuaFn)});
    assert(segs.size() == 1);
    assert(segs[0].native);
    assert(segs[0].idx.size() == 1 && segs[0].idx[0] == 0);
}

static void test_single_external() {
    auto segs = build_segments({mk(CmdKind::External)});
    assert(segs.size() == 1);
    assert(!segs[0].native);
}

static void test_native_then_external() {
    auto segs = build_segments({mk(CmdKind::LuaFn), mk(CmdKind::External)});
    assert(segs.size() == 2);
    assert(segs[0].native && segs[0].idx == std::vector<int>{0});
    assert(!segs[1].native && segs[1].idx == std::vector<int>{1});
}

static void test_external_then_native() {
    auto segs = build_segments({mk(CmdKind::External), mk(CmdKind::Builtin)});
    assert(segs.size() == 2);
    assert(!segs[0].native);
    assert(segs[1].native);
}

static void test_two_natives_grouped_then_external() {
    auto segs = build_segments({mk(CmdKind::LuaFn), mk(CmdKind::Builtin), mk(CmdKind::External)});
    assert(segs.size() == 2);
    assert(segs[0].native);
    assert(segs[0].idx.size() == 2 && segs[0].idx[0] == 0 && segs[0].idx[1] == 1);
    assert(!segs[1].native);
}

static void test_native_external_native_external() {
    auto segs = build_segments({mk(CmdKind::LuaFn), mk(CmdKind::External),
                                 mk(CmdKind::Builtin), mk(CmdKind::External)});
    assert(segs.size() == 4);
    assert(segs[0].native && segs[0].idx == std::vector<int>{0});
    assert(!segs[1].native && segs[1].idx == std::vector<int>{1});
    assert(segs[2].native && segs[2].idx == std::vector<int>{2});
    assert(!segs[3].native && segs[3].idx == std::vector<int>{3});
}

// A brace group with no comma and no range is not an expansion -- `git show
// HEAD@{1}` and `awk {print}` have to survive intact.
static void test_brace_left_alone() {
    assert(brace_expand("HEAD@{1}") == std::vector<std::string>{"HEAD@{1}"});
    assert(brace_expand("{print}") == std::vector<std::string>{"{print}"});
    assert(brace_expand("plain") == std::vector<std::string>{"plain"});
    assert(brace_expand("{unclosed") == std::vector<std::string>{"{unclosed"});
    assert(brace_expand("a{1}b{c,d}") == (std::vector<std::string>{"a{1}bc", "a{1}bd"}));
}

static void test_brace_alternatives() {
    assert(brace_expand("{a,b}") == (std::vector<std::string>{"a", "b"}));
    assert(brace_expand("x{a,b}y") == (std::vector<std::string>{"xay", "xby"}));
    assert(brace_expand("{a,b}{1,2}") ==
           (std::vector<std::string>{"a1", "a2", "b1", "b2"}));
    assert(brace_expand("{a,{b,c}}") == (std::vector<std::string>{"a", "b", "c"}));
    assert(brace_expand("{a,}") == (std::vector<std::string>{"a", ""}));
}

static void test_brace_ranges() {
    assert(brace_expand("{1..4}") == (std::vector<std::string>{"1", "2", "3", "4"}));
    assert(brace_expand("{3..1}") == (std::vector<std::string>{"3", "2", "1"}));
    assert(brace_expand("f{a..c}") == (std::vector<std::string>{"fa", "fb", "fc"}));
    assert(brace_expand("{-1..1}") == (std::vector<std::string>{"-1", "0", "1"}));
    // Not a range: left as written rather than guessed at.
    assert(brace_expand("{a..bc}") == std::vector<std::string>{"{a..bc}"});
    assert(brace_expand("{1..2..3}") == std::vector<std::string>{"{1..2..3}"});
}

static Process running_proc() {
    Process p;
    p.pid = 100;
    return p;
}

static void test_job_all_completed() {
    ShellJob j;
    j.procs.push_back(running_proc());
    assert(!j.all_completed());
    j.procs[0].completed = true;
    assert(j.all_completed());
    assert(!j.all_stopped());
}

static void test_job_all_stopped() {
    ShellJob j;
    j.procs.push_back(running_proc());
    j.procs.push_back(running_proc());
    j.procs[0].stopped = true;
    assert(!j.all_stopped()); // second proc still running
    j.procs[1].stopped = true;
    assert(j.all_stopped());
    assert(!j.all_completed());
}

static void test_format_job_line() {
    ShellJob j;
    j.id = 3;
    j.cmdline = "sleep 100";
    j.procs.push_back(running_proc());
    std::string line = format_job_line(j);
    assert(line.find("[3]") != std::string::npos);
    assert(line.find("Running") != std::string::npos);
    assert(line.find("sleep 100") != std::string::npos);

    j.procs[0].completed = true;
    line = format_job_line(j);
    assert(line.find("Done") != std::string::npos);
}

int main() {
    test_single_native();
    test_single_external();
    test_native_then_external();
    test_external_then_native();
    test_two_natives_grouped_then_external();
    test_native_external_native_external();
    test_brace_left_alone();
    test_brace_alternatives();
    test_brace_ranges();
    test_job_all_completed();
    test_job_all_stopped();
    test_format_job_line();
    std::puts("test_executor: all tests passed");
    return 0;
}
