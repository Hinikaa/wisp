#pragma once

#include <string>
#include <vector>

// Bare command-line tokenizer. No $VAR/$(cmd) expansion here -- that happens
// at execution time in executor.cpp, since $(cmd) requires actually running
// something and this module must stay pure (testable on a plain string).
enum class TokType {
    Word, Pipe, Less, Great, DGreat, ErrGreat, AndAnd, OrOr, Semi, Amp, End
};

struct Token {
    TokType type;
    std::string text;      // set for Word tokens
    bool literal = false;  // true if single-quoted: never subject to $ expansion
};

// Throws std::runtime_error on an unterminated quote.
std::vector<Token> lex(const std::string& line);
