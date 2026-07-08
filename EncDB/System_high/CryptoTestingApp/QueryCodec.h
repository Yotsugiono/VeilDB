#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "EncDBRequest.h"

enum class ASTNodeType {
    TERM,
    AND,
    OR,
    NOT
};

struct ASTNode {
    ASTNodeType type = ASTNodeType::TERM;
    int term_index = 0;
    std::unique_ptr<ASTNode> left;
    std::unique_ptr<ASTNode> right;
};

class BoolExprParser {
public:
    explicit BoolExprParser(const std::vector<std::string>& tokens)
        : tokens_(tokens) {}

    std::unique_ptr<ASTNode> parse_expr() {
        auto node = parse_term();
        while (match("OR")) {
            node = make_node(ASTNodeType::OR, std::move(node), parse_term());
        }
        return node;
    }

    const std::vector<std::string>& keywords() const {
        return keywords_;
    }

private:
    const std::vector<std::string>& tokens_;
    size_t pos_ = 0;
    std::vector<std::string> keywords_;

    std::unique_ptr<ASTNode> parse_term() {
        auto node = parse_unary();
        while (match("AND")) {
            node = make_node(ASTNodeType::AND, std::move(node), parse_unary());
        }
        return node;
    }

    std::unique_ptr<ASTNode> parse_unary() {
        if (match("NOT")) {
            return make_node(ASTNodeType::NOT, parse_unary(), nullptr);
        }
        return parse_factor();
    }

    std::unique_ptr<ASTNode> parse_factor() {
        if (match("(")) {
            auto node = parse_expr();
            expect(")");
            return node;
        }
        return parse_word();
    }

    std::unique_ptr<ASTNode> parse_word() {
        if (pos_ >= tokens_.size()) {
            throw std::runtime_error("unexpected end");
        }

        auto node = std::make_unique<ASTNode>();
        node->type = ASTNodeType::TERM;

        auto keyword_it = std::find(keywords_.begin(), keywords_.end(), tokens_[pos_]);
        if (keyword_it == keywords_.end()) {
            keywords_.push_back(tokens_[pos_]);
            keyword_it = std::prev(keywords_.end());
        }

        node->term_index = static_cast<int>(std::distance(keywords_.begin(), keyword_it));
        ++pos_;
        return node;
    }

    bool match(const std::string& token) {
        if (pos_ < tokens_.size() && tokens_[pos_] == token) {
            ++pos_;
            return true;
        }
        return false;
    }

    void expect(const std::string& token) {
        if (!match(token)) {
            throw std::runtime_error("expect " + token);
        }
    }

    static std::unique_ptr<ASTNode> make_node(
        ASTNodeType type,
        std::unique_ptr<ASTNode> left,
        std::unique_ptr<ASTNode> right
    ) {
        auto node = std::make_unique<ASTNode>();
        node->type = type;
        node->left = std::move(left);
        node->right = std::move(right);
        return node;
    }
};

inline std::string to_upper(const std::string& input) {
    std::string result = input;
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    return result;
}

inline std::vector<std::string> tokenize_query(const std::string& input) {
    std::vector<std::string> tokens;
    std::string current;

    auto flush = [&]() {
        if (!current.empty()) {
            tokens.push_back(current);
            current.clear();
        }
    };

    for (unsigned char ch : input) {
        if (std::isspace(ch)) {
            flush();
        } else if (ch == '(' || ch == ')') {
            flush();
            tokens.emplace_back(1, static_cast<char>(ch));
        } else {
            current.push_back(static_cast<char>(ch));
        }
    }

    flush();
    return tokens;
}

inline EncTerm make_term(const std::string& keyword) {
    EncTerm term{};
    const size_t term_len = std::min(keyword.size(), static_cast<size_t>(MAX_KEYWORD_LEN));
    std::memset(&term, 0, sizeof(term));
    std::memcpy(term.data, keyword.data(), term_len);
    term.len = static_cast<uint8_t>(term_len);
    return term;
}

inline void encode_bool_expr(const ASTNode* node, uint8_t* out, int& len) {
    if (!node) {
        return;
    }

    encode_bool_expr(node->left.get(), out, len);
    encode_bool_expr(node->right.get(), out, len);

    switch (node->type) {
    case ASTNodeType::TERM:
        out[len++] = OP_TERM;
        out[len++] = static_cast<uint8_t>(node->term_index);
        break;
    case ASTNodeType::AND:
        out[len++] = OP_AND;
        break;
    case ASTNodeType::OR:
        out[len++] = OP_OR;
        break;
    case ASTNodeType::NOT:
        out[len++] = OP_NOT;
        break;
    }
}
