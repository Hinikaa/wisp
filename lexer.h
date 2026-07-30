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
    // Per-character protection mask, same length as text: '1' marks a
    // character that came from a single-quoted section or a backslash escape
    // and so must survive $/glob/brace expansion untouched. A per-character
    // mask beats one bool per word because quoting is per-section --
    // `--fmt='%h %s'` is a single argument with only its tail protected.
    std::string prot;

    bool literal() const { return prot.find('0') == std::string::npos; }
};

// Index of the ')' closing the '(' at `open`, or npos. Shared with the
// expander in executor.cpp, which has to find the same bounds again at
// substitution time. Parens inside quotes are not discounted -- seeing
// through `$(echo ")")` needs a recursive lexer, which this isn't.
size_t match_paren(const std::string& s, size_t open);

// Throws std::runtime_error on an unterminated quote or $( .
std::vector<Token> lex(const std::string& line);
