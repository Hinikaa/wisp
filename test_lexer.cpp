#include <cassert>
#include <cstdio>
#include <stdexcept>

#include "lexer.h"

static void test_bare_words() {
    auto toks = lex("ls -la foo");
    assert(toks.size() == 4); // ls, -la, foo, End
    assert(toks[0].type == TokType::Word && toks[0].text == "ls" && !toks[0].literal);
    assert(toks[1].type == TokType::Word && toks[1].text == "-la");
    assert(toks[2].type == TokType::Word && toks[2].text == "foo");
    assert(toks[3].type == TokType::End);
}

static void test_single_quote_literal() {
    auto toks = lex("echo '$HOME'");
    assert(toks.size() == 3);
    assert(toks[1].type == TokType::Word && toks[1].text == "$HOME" && toks[1].literal == true);
}

static void test_double_quote_not_literal() {
    auto toks = lex("echo \"$HOME\"");
    assert(toks[1].type == TokType::Word && toks[1].text == "$HOME" && toks[1].literal == false);
}

static void test_double_quote_escapes() {
    auto toks = lex("echo \"a\\\"b\\\\c\"");
    assert(toks[1].type == TokType::Word && toks[1].text == "a\"b\\c");
}

static void test_operators() {
    auto toks = lex("a | b && c || d ; e & f < g > h >> i");
    std::vector<TokType> want = {
        TokType::Word, TokType::Pipe, TokType::Word, TokType::AndAnd, TokType::Word,
        TokType::OrOr, TokType::Word, TokType::Semi, TokType::Word, TokType::Amp,
        TokType::Word, TokType::Less, TokType::Word, TokType::Great, TokType::Word,
        TokType::DGreat, TokType::Word, TokType::End,
    };
    assert(toks.size() == want.size());
    for (size_t i = 0; i < want.size(); ++i) assert(toks[i].type == want[i]);
}

static void test_stderr_redirect_no_space() {
    auto toks = lex("cmd 2> err.log");
    assert(toks.size() == 4);
    assert(toks[1].type == TokType::ErrGreat);
    assert(toks[2].type == TokType::Word && toks[2].text == "err.log");
}

static void test_two_then_great_with_space_is_word_then_redirect() {
    auto toks = lex("echo 2 > file");
    assert(toks.size() == 5);
    assert(toks[1].type == TokType::Word && toks[1].text == "2");
    assert(toks[2].type == TokType::Great);
    assert(toks[3].type == TokType::Word && toks[3].text == "file");
}

static void test_no_space_adjacency() {
    auto toks = lex("echo a|echo b");
    std::vector<TokType> want = {TokType::Word, TokType::Word, TokType::Pipe,
                                  TokType::Word, TokType::Word, TokType::End};
    assert(toks.size() == want.size());
    for (size_t i = 0; i < want.size(); ++i) assert(toks[i].type == want[i]);
}

static void test_unterminated_single_quote_throws() {
    bool threw = false;
    try { lex("echo 'unterminated"); }
    catch (const std::runtime_error&) { threw = true; }
    assert(threw);
}

static void test_unterminated_double_quote_throws() {
    bool threw = false;
    try { lex("echo \"unterminated"); }
    catch (const std::runtime_error&) { threw = true; }
    assert(threw);
}

int main() {
    test_bare_words();
    test_single_quote_literal();
    test_double_quote_not_literal();
    test_double_quote_escapes();
    test_operators();
    test_stderr_redirect_no_space();
    test_two_then_great_with_space_is_word_then_redirect();
    test_no_space_adjacency();
    test_unterminated_single_quote_throws();
    test_unterminated_double_quote_throws();
    std::puts("test_lexer: all tests passed");
    return 0;
}
