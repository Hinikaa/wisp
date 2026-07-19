#include "parser.h"

#include <stdexcept>

namespace {

class Cursor {
public:
    explicit Cursor(const std::vector<Token>& toks) : toks_(toks) {}

    const Token& peek() const { return toks_[pos_]; }
    const Token& advance() { return toks_[pos_++]; }

    bool at_end() const { return peek().type == TokType::End; }

private:
    const std::vector<Token>& toks_;
    size_t pos_ = 0;
};

RedirKind redir_kind_for(TokType t) {
    switch (t) {
        case TokType::Less: return RedirKind::In;
        case TokType::Great: return RedirKind::Out;
        case TokType::DGreat: return RedirKind::Append;
        case TokType::ErrGreat: return RedirKind::ErrOut;
        case TokType::ErrToOut: return RedirKind::ErrToOut;
        default: throw std::runtime_error("wisp: internal error: not a redirect token");
    }
}

bool is_redirect_tok(TokType t) {
    return t == TokType::Less || t == TokType::Great || t == TokType::DGreat || t == TokType::ErrGreat || t == TokType::ErrToOut;
}

const char* tok_type_name(TokType t) {
    switch (t) {
        case TokType::Pipe: return "|";
        case TokType::Less: return "<";
        case TokType::Great: return ">";
        case TokType::DGreat: return ">>";
        case TokType::ErrGreat: return "2>";
        case TokType::ErrToOut: return "2>&1";
        case TokType::AndAnd: return "&&";
        case TokType::OrOr: return "||";
        case TokType::Semi: return ";";
        case TokType::Amp: return "&";
        case TokType::End: return "end of input";
        case TokType::Word: return ""; // handled separately
    }
    return "";
}

Command parse_command(Cursor& c) {
    Command cmd;
    while (true) {
        TokType t = c.peek().type;
        if (t == TokType::Word) {
            const Token& tok = c.advance();
            cmd.argv.push_back(Word{tok.text, tok.literal});
        } else if (is_redirect_tok(t)) {
            RedirKind kind = redir_kind_for(t);
            c.advance();
            if (kind == RedirKind::ErrToOut) {
                cmd.redirects.push_back(Redirect{kind, Word{""}});
            } else {
                if (c.peek().type != TokType::Word) {
                    if (c.peek().type == TokType::End)
                        throw std::runtime_error("wisp: unexpected end of input after redirect");
                    throw std::runtime_error(std::string("wisp: expected a filename after redirect, got ") + tok_type_name(c.peek().type));
                }
                const Token& target = c.advance();
                cmd.redirects.push_back(Redirect{kind, Word{target.text, target.literal}});
            }
        } else {
            break;
        }
    }
    if (cmd.argv.empty())
        throw std::runtime_error("wisp: expected a command");
    return cmd;
}

Pipeline parse_pipeline(Cursor& c) {
    Pipeline pl;
    pl.commands.push_back(parse_command(c));
    while (c.peek().type == TokType::Pipe) {
        c.advance();
        pl.commands.push_back(parse_command(c));
    }
    return pl;
}

AndOr parse_and_or(Cursor& c) {
    AndOr ao;
    ao.parts.push_back(AndOrPart{parse_pipeline(c), BoolOp::None});
    while (c.peek().type == TokType::AndAnd || c.peek().type == TokType::OrOr) {
        BoolOp op = c.peek().type == TokType::AndAnd ? BoolOp::And : BoolOp::Or;
        c.advance();
        ao.parts.back().op_after = op;
        ao.parts.push_back(AndOrPart{parse_pipeline(c), BoolOp::None});
    }
    return ao;
}

Job parse_job(Cursor& c) {
    Job job;
    job.and_or = parse_and_or(c);
    if (c.peek().type == TokType::Amp) {
        job.background = true;
        c.advance();
    } else if (c.peek().type == TokType::Semi) {
        c.advance();
    }
    return job;
}

} // namespace

Line parse(const std::vector<Token>& tokens) {
    Cursor c(tokens);
    Line line;
    while (!c.at_end()) {
        line.jobs.push_back(parse_job(c));
    }
    return line;
}
