#pragma once

#include <string>
#include <vector>

#include "lexer.h"

// grammar:
//   line     := job (job_sep job)* job_sep?      job_sep := ';' | '&'
//   job      := pipeline (('&&'|'||') pipeline)*
//   pipeline := command ('|' command)*
//   command  := word+ redirect*                  redirect: < > >> 2>
//
// Pure: builds an AST from tokens, no expansion, no execution. $VAR/$(cmd)
// expansion happens later in executor.cpp, at execution time.

struct Word {
    std::string text;
    bool literal = false; // single-quoted: never expanded
};

enum class RedirKind { In, Out, Append, ErrOut };

struct Redirect {
    RedirKind kind;
    Word target;
};

struct Command {
    std::vector<Word> argv;
    std::vector<Redirect> redirects;
};

struct Pipeline {
    std::vector<Command> commands;
};

enum class BoolOp { None, And, Or };

// A pipeline plus the operator joining it to the *next* part (None on the
// last part of an AndOr).
struct AndOrPart {
    Pipeline pipeline;
    BoolOp op_after = BoolOp::None;
};

struct AndOr {
    std::vector<AndOrPart> parts;
};

struct Job {
    AndOr and_or;
    bool background = false;
};

struct Line {
    std::vector<Job> jobs;
};

// Throws std::runtime_error on malformed input.
Line parse(const std::vector<Token>& tokens);

inline Line parse_line(const std::string& s) { return parse(lex(s)); }
