#pragma once

#include <string>
#include <vector>

// Bare command-line tokenizer. No $VAR/$(cmd) expansion here -- that happens
// at execution time in executor.cpp, since $(cmd) requires actually running
// something and this module must stay pure (testable on a plain string).
enum class TokType {
    Word, Pipe, Less, Great, DGreat, ErrGreat, ErrToOut, AmpGreat, AndAnd, OrOr, Semi, Amp, End
};

struct Token {
    TokType type;
    std::string text;  // set for Word tokens
    // Per-character protection mask, same length as text:
    //   '0' = bare unquoted (eligible for $, ~, brace, and glob expansion)
    //   '1' = single-quoted or backslash-escaped (completely literal)
    //   '2' = double-quoted ($ expands; ~, brace, and glob stay literal)
    // A per-character mask beats one bool per word because quoting is
    // per-section -- `--fmt='%h %s'` is one argument with only its tail '1'.
    std::string prot;

    bool literal() const { return prot.find_first_not_of('1') == std::string::npos; }
};

// Index of the ')' closing the '(' at `open`, or npos. Shared with the
// expander in executor.cpp, which has to find the same bounds again at
// substitution time. Parens inside quotes are not discounted -- seeing
// through `$(echo ")")` needs a recursive lexer, which this isn't.
size_t match_paren(const std::string& s, size_t open);

// Throws std::runtime_error on an unterminated quote or $( .
std::vector<Token> lex(const std::string& line);
