#include <cassert>
#include <cstdio>
#include <stdexcept>

#include "parser.h"

static void test_single_command() {
    Line l = parse_line("ls -la");
    assert(l.jobs.size() == 1);
    auto& job = l.jobs[0];
    assert(!job.background);
    assert(job.and_or.parts.size() == 1);
    auto& pl = job.and_or.parts[0].pipeline;
    assert(pl.commands.size() == 1);
    assert(pl.commands[0].argv.size() == 2);
    assert(pl.commands[0].argv[0].text == "ls");
    assert(pl.commands[0].argv[1].text == "-la");
}

static void test_pipeline_three_stages() {
    Line l = parse_line("ls | grep foo | wc -l");
    auto& pl = l.jobs[0].and_or.parts[0].pipeline;
    assert(pl.commands.size() == 3);
    assert(pl.commands[0].argv[0].text == "ls");
    assert(pl.commands[1].argv[0].text == "grep");
    assert(pl.commands[2].argv[1].text == "-l");
}

static void test_redirects_with_pipe() {
    Line l = parse_line("cmd < in.txt | cmd2 > out.txt 2> err.txt");
    auto& pl = l.jobs[0].and_or.parts[0].pipeline;
    assert(pl.commands[0].redirects.size() == 1);
    assert(pl.commands[0].redirects[0].kind == RedirKind::In);
    assert(pl.commands[0].redirects[0].target.text == "in.txt");
    assert(pl.commands[1].redirects.size() == 2);
    assert(pl.commands[1].redirects[0].kind == RedirKind::Out);
    assert(pl.commands[1].redirects[1].kind == RedirKind::ErrOut);
}

static void test_and_or_chain() {
    Line l = parse_line("a && b || c");
    auto& parts = l.jobs[0].and_or.parts;
    assert(parts.size() == 3);
    assert(parts[0].op_after == BoolOp::And);
    assert(parts[1].op_after == BoolOp::Or);
    assert(parts[2].op_after == BoolOp::None);
}

static void test_semicolon_sequencing() {
    Line l = parse_line("a ; b ; c");
    assert(l.jobs.size() == 3);
    for (auto& j : l.jobs) assert(!j.background);
}

static void test_trailing_ampersand_sets_background() {
    Line l = parse_line("sleep 5 &");
    assert(l.jobs.size() == 1);
    assert(l.jobs[0].background);
}

static void test_mixed_sequencing() {
    Line l = parse_line("a & b ; c");
    assert(l.jobs.size() == 3);
    assert(l.jobs[0].background);
    assert(!l.jobs[1].background);
    assert(!l.jobs[2].background);
}

static void test_malformed_dangling_pipe_throws() {
    bool threw = false;
    try { parse_line("ls |"); }
    catch (const std::runtime_error&) { threw = true; }
    assert(threw);
}

static void test_malformed_redirect_no_target_throws() {
    bool threw = false;
    try { parse_line("ls >"); }
    catch (const std::runtime_error&) { threw = true; }
    assert(threw);
}

static void test_malformed_empty_command_throws() {
    bool threw = false;
    try { parse_line("&& b"); }
    catch (const std::runtime_error&) { threw = true; }
    assert(threw);
}

int main() {
    test_single_command();
    test_pipeline_three_stages();
    test_redirects_with_pipe();
    test_and_or_chain();
    test_semicolon_sequencing();
    test_trailing_ampersand_sets_background();
    test_mixed_sequencing();
    test_malformed_dangling_pipe_throws();
    test_malformed_redirect_no_target_throws();
    test_malformed_empty_command_throws();
    std::puts("test_parser: all tests passed");
    return 0;
}
