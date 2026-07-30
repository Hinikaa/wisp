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

size_t match_paren(const std::string& s, size_t open) {
    int depth = 0;
    for (size_t i = open; i < s.size(); ++i) {
        if (s[i] == '(') ++depth;
        else if (s[i] == ')' && --depth == 0) return i;
    }
    return std::string::npos;
}

std::vector<Token> lex(const std::string& line) {
    std::vector<Token> out;
    size_t pos = 0;
    size_t word_end = std::string::npos; // end offset of the last Word emitted
    const size_t n = line.size();

    // Adjacent word chunks with nothing between them are one argument:
    // `--fmt='%h'` must reach argv whole, not as `--fmt=` plus `%h`. Called
    // with pos already past the chunk.
    auto push_word = [&](size_t start, const std::string& text, const std::string& prot) {
        if (!out.empty() && out.back().type == TokType::Word && start == word_end) {
            out.back().text += text;
            out.back().prot += prot;
        } else {
            out.push_back({TokType::Word, text, prot});
        }
        word_end = pos;
    };

    while (pos < n) {
        char c = line[pos];

        if (c == ' ' || c == '\t') { ++pos; continue; }

        if (c == '|') {
            if (pos + 1 < n && line[pos + 1] == '|') { out.push_back({TokType::OrOr, "", ""}); pos += 2; }
            else { out.push_back({TokType::Pipe, "", ""}); pos += 1; }
            continue;
        }
        if (c == '&') {
            if (pos + 1 < n && line[pos + 1] == '&') { out.push_back({TokType::AndAnd, "", ""}); pos += 2; }
            else if (pos + 1 < n && line[pos + 1] == '>') { out.push_back({TokType::AmpGreat, "", ""}); pos += 2; }
            else { out.push_back({TokType::Amp, "", ""}); pos += 1; }
            continue;
        }
        if (c == ';') { out.push_back({TokType::Semi, "", ""}); pos += 1; continue; }
        if (c == '<') { out.push_back({TokType::Less, "", ""}); pos += 1; continue; }
        if (c == '>') {
            if (pos + 1 < n && line[pos + 1] == '>') { out.push_back({TokType::DGreat, "", ""}); pos += 2; }
            else { out.push_back({TokType::Great, "", ""}); pos += 1; }
            continue;
        }
        if (c == '\'') {
            size_t start = pos++;
            size_t end = line.find('\'', pos);
            if (end == std::string::npos)
                throw std::runtime_error("wisp: unterminated single-quoted string");
            std::string text = line.substr(pos, end - pos);
            pos = end + 1;
            push_word(start, text, std::string(text.size(), '1'));
            continue;
        }
        if (c == '"') {
            size_t start = pos++;
            std::string text, prot;
            bool closed = false;
            while (pos < n) {
                char d = line[pos];
                if (d == '"') { closed = true; ++pos; break; }
                if (d == '\\' && pos + 1 < n) {
                    char e = line[pos + 1];
                    if (e == '"' || e == '\\' || e == '$' || e == '`') {
                        text.push_back(e);
                        prot.push_back('1');
                        pos += 2;
                        continue;
                    }
                }
                text.push_back(d);
                prot.push_back('0');
                ++pos;
            }
            if (!closed) throw std::runtime_error("wisp: unterminated double-quoted string");
            push_word(start, text, prot);
            continue;
        }

        // A '#' opening a word starts a comment; inside a word ("a#b") it is
        // just a character, same as bash.
        if (c == '#' && pos != word_end) break;

        // Bare word.
        size_t start = pos;
        std::string text, prot;
        while (pos < n) {
            char d = line[pos];
            if (d == '\\' && pos + 1 < n) {
                text.push_back(line[pos + 1]);
                prot.push_back('1');
                pos += 2;
                continue;
            }
            // $( ... ) is carried through whole, spaces and pipes included --
            // the expander runs it as its own command line later, and word
            // stops inside it belong to that line, not to this one.
            if (d == '$' && pos + 1 < n && line[pos + 1] == '(') {
                size_t close = match_paren(line, pos + 1);
                if (close == std::string::npos)
                    throw std::runtime_error("wisp: unterminated $( in command substitution");
                text.append(line, pos, close - pos + 1);
                prot.append(close - pos + 1, '0');
                pos = close + 1;
                continue;
            }
            if (is_word_stop(d)) break;
            text.push_back(d);
            prot.push_back('0');
            ++pos;
        }

        // "2>" (no space) is the stderr-redirect operator; "2 >" (space) is
        // the literal word "2" followed by a separate '>' token -- already
        // handled naturally since the space breaks adjacency before we get
        // here. Only collapse when the bare word is exactly "2" and '>'
        // immediately follows with zero gap.
        if (text == "2" && start != word_end && pos < n && line[pos] == '>') {
            ++pos;
            if (pos + 1 < n && line[pos] == '&' && line[pos + 1] == '1') {
                pos += 2;
                out.push_back({TokType::ErrToOut, "", ""});
            } else {
                out.push_back({TokType::ErrGreat, "", ""});
            }
            continue;
        }

        push_word(start, text, prot);
    }

    out.push_back({TokType::End, "", ""});
    return out;
}
