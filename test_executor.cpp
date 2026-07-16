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
    test_job_all_completed();
    test_job_all_stopped();
    test_format_job_line();
    std::puts("test_executor: all tests passed");
    return 0;
}
