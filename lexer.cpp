#include "lexer.h"

#include <stdexcept>

namespace {

constexpr const char* kWordStop = " \t|<>&;'\"";

bool is_word_stop(char c) {
    for (const char* p = kWordStop; *p; ++p)
        if (*p == c) return true;
    return false;
}

} // namespace

std::vector<Token> lex(const std::string& line) {
    std::vector<Token> out;
    size_t pos = 0;
    const size_t n = line.size();

    while (pos < n) {
        char c = line[pos];

        if (c == ' ' || c == '\t') { ++pos; continue; }

        if (c == '|') {
            if (pos + 1 < n && line[pos + 1] == '|') { out.push_back({TokType::OrOr, "", false}); pos += 2; }
            else { out.push_back({TokType::Pipe, "", false}); pos += 1; }
            continue;
        }
        if (c == '&') {
            if (pos + 1 < n && line[pos + 1] == '&') { out.push_back({TokType::AndAnd, "", false}); pos += 2; }
            else { out.push_back({TokType::Amp, "", false}); pos += 1; }
            continue;
        }
        if (c == ';') { out.push_back({TokType::Semi, "", false}); pos += 1; continue; }
        if (c == '<') { out.push_back({TokType::Less, "", false}); pos += 1; continue; }
        if (c == '>') {
            if (pos + 1 < n && line[pos + 1] == '>') { out.push_back({TokType::DGreat, "", false}); pos += 2; }
            else { out.push_back({TokType::Great, "", false}); pos += 1; }
            continue;
        }
        if (c == '\'') {
            size_t start = ++pos;
            size_t end = line.find('\'', start);
            if (end == std::string::npos)
                throw std::runtime_error("wisp: unterminated single-quoted string");
            out.push_back({TokType::Word, line.substr(start, end - start), true});
            pos = end + 1;
            continue;
        }
        if (c == '"') {
            ++pos;
            std::string text;
            bool closed = false;
            while (pos < n) {
                char d = line[pos];
                if (d == '"') { closed = true; ++pos; break; }
                if (d == '\\' && pos + 1 < n && (line[pos + 1] == '"' || line[pos + 1] == '\\')) {
                    text.push_back(line[pos + 1]);
                    pos += 2;
                    continue;
                }
                text.push_back(d);
                ++pos;
            }
            if (!closed) throw std::runtime_error("wisp: unterminated double-quoted string");
            out.push_back({TokType::Word, text, false});
            continue;
        }

        // Bare word.
        size_t start = pos;
        while (pos < n && !is_word_stop(line[pos])) ++pos;
        std::string text = line.substr(start, pos - start);

        // "2>" (no space) is the stderr-redirect operator; "2 >" (space) is
        // the literal word "2" followed by a separate '>' token -- already
        // handled naturally since the space breaks adjacency before we get
        // here. Only collapse when the bare word is exactly "2" and '>'
        // immediately follows with zero gap.
        if (text == "2" && pos < n && line[pos] == '>') {
            ++pos;
            out.push_back({TokType::ErrGreat, "", false});
            continue;
        }

        out.push_back({TokType::Word, text, false});
    }

    out.push_back({TokType::End, "", false});
    return out;
}
