// query_parser.cpp - lexer and recursive-descent parser for the query language.
//
// The lexer and parser are both internal to this translation unit; neither is
// exposed through the public header. QueryEngine::parse() is the only external
// entry point.
//
// Lexer handles:
//   Keywords (case-insensitive): kind field has_child has_ref alive any AND OR NOT
//   Operators: = != < > <= >=
//   Punctuation: ( ) :
//   Quoted strings: "..." -> STRING (content without quotes, no escape sequences)
//   Identifiers: [a-zA-Z_][a-zA-Z0-9_]* -> IDENT
//   Integers: [-]?[0-9]+ -> INTEGER
//   Whitespace: skipped
//
// Grammar:
//   expr      := or_expr
//   or_expr   := and_expr ('OR' and_expr)*
//   and_expr  := not_expr ('AND' not_expr)*
//   not_expr  := 'NOT' not_expr | atom_expr
//   atom_expr := 'kind' ':' ident
//              | 'field' ':' ident cmp_op value
//              | 'has_child' ':' ident
//              | 'has_ref' ':' ident
//              | 'alive'
//              | 'any'
//              | '(' expr ')'
//   cmp_op    := '=' | '!=' | '<' | '>' | '<=' | '>='
//   value     := STRING | INTEGER

#include "stratavm/query.hpp"
#include "stratavm/atom_pool.hpp"
#include "stratavm/schema.hpp"
#include "stratavm/node.hpp"
#include "stratavm/status.hpp"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace stratavm {

// ===========================================================================
// Lexer (private to this TU)
// ===========================================================================

class QueryLexer {
 public:
  explicit QueryLexer(const std::string& src)
      : src_(src), pos_(0) {}

  // Tokenise the entire input and store tokens in `out`.
  // Returns an error Status on illegal characters.
  Status tokenize(std::vector<QueryToken>& out) {
    while (pos_ < src_.size()) {
      skip_ws();
      if (pos_ >= src_.size()) break;

      std::size_t tok_start = pos_;
      char c = src_[pos_];

      // String literal
      if (c == '"') {
        QueryToken t;
        t.pos = tok_start;
        t.kind = QueryTokenKind::STRING;
        ++pos_;  // consume opening quote
        while (pos_ < src_.size() && src_[pos_] != '"') {
          t.string_val += src_[pos_];
          ++pos_;
        }
        if (pos_ >= src_.size()) {
          return fail(Code::BadValue,
                      "unterminated string literal at pos " +
                          std::to_string(tok_start));
        }
        ++pos_;  // consume closing quote
        out.push_back(std::move(t));
        continue;
      }

      // Two-character operators: != <= >=
      if (c == '!' && pos_ + 1 < src_.size() && src_[pos_ + 1] == '=') {
        QueryToken t;
        t.pos = tok_start;
        t.kind = QueryTokenKind::NEQ;
        pos_ += 2;
        out.push_back(std::move(t));
        continue;
      }
      if (c == '<' && pos_ + 1 < src_.size() && src_[pos_ + 1] == '=') {
        QueryToken t;
        t.pos = tok_start;
        t.kind = QueryTokenKind::LE;
        pos_ += 2;
        out.push_back(std::move(t));
        continue;
      }
      if (c == '>' && pos_ + 1 < src_.size() && src_[pos_ + 1] == '=') {
        QueryToken t;
        t.pos = tok_start;
        t.kind = QueryTokenKind::GE;
        pos_ += 2;
        out.push_back(std::move(t));
        continue;
      }

      // Single-character operators / punctuation
      if (c == '=') { push_single(out, QueryTokenKind::EQ,     tok_start); continue; }
      if (c == '<') { push_single(out, QueryTokenKind::LT,     tok_start); continue; }
      if (c == '>') { push_single(out, QueryTokenKind::GT,     tok_start); continue; }
      if (c == ':') { push_single(out, QueryTokenKind::COLON,  tok_start); continue; }
      if (c == '(') { push_single(out, QueryTokenKind::LPAREN, tok_start); continue; }
      if (c == ')') { push_single(out, QueryTokenKind::RPAREN, tok_start); continue; }

      // Negative integer: starts with '-' followed by digit
      if (c == '-' && pos_ + 1 < src_.size() &&
          std::isdigit(static_cast<unsigned char>(src_[pos_ + 1]))) {
        QueryToken t;
        t.pos = tok_start;
        t.kind = QueryTokenKind::INTEGER;
        std::string num;
        num += src_[pos_++];  // '-'
        while (pos_ < src_.size() &&
               std::isdigit(static_cast<unsigned char>(src_[pos_]))) {
          num += src_[pos_++];
        }
        t.int_val = std::stoll(num);
        out.push_back(std::move(t));
        continue;
      }

      // Positive integer
      if (std::isdigit(static_cast<unsigned char>(c))) {
        QueryToken t;
        t.pos = tok_start;
        t.kind = QueryTokenKind::INTEGER;
        std::string num;
        while (pos_ < src_.size() &&
               std::isdigit(static_cast<unsigned char>(src_[pos_]))) {
          num += src_[pos_++];
        }
        t.int_val = std::stoll(num);
        out.push_back(std::move(t));
        continue;
      }

      // Identifier or keyword
      if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
        QueryToken t;
        t.pos = tok_start;
        std::string word;
        while (pos_ < src_.size() &&
               (std::isalnum(static_cast<unsigned char>(src_[pos_])) ||
                src_[pos_] == '_')) {
          word += src_[pos_++];
        }
        t.kind = classify_word(word);
        if (t.kind == QueryTokenKind::IDENT) {
          t.string_val = std::move(word);
        }
        out.push_back(std::move(t));
        continue;
      }

      // Unknown character
      return fail(Code::BadValue,
                  std::string("unexpected character '") + c +
                      "' at pos " + std::to_string(tok_start));
    }

    // Append END sentinel
    QueryToken end_tok;
    end_tok.kind = QueryTokenKind::END;
    end_tok.pos = src_.size();
    out.push_back(std::move(end_tok));
    return Status::ok();
  }

 private:
  const std::string& src_;
  std::size_t pos_;

  void skip_ws() {
    while (pos_ < src_.size() &&
           std::isspace(static_cast<unsigned char>(src_[pos_]))) {
      ++pos_;
    }
  }

  void push_single(std::vector<QueryToken>& out,
                   QueryTokenKind k,
                   std::size_t pos) {
    QueryToken t;
    t.kind = k;
    t.pos = pos;
    ++pos_;
    out.push_back(std::move(t));
  }

  // Map a word to its keyword token kind (case-insensitive for AND/OR/NOT).
  static QueryTokenKind classify_word(const std::string& w) {
    // Exact lowercase keywords first
    if (w == "kind")      return QueryTokenKind::KIND;
    if (w == "field")     return QueryTokenKind::FIELD;
    if (w == "has_child") return QueryTokenKind::HAS_CHILD;
    if (w == "has_ref")   return QueryTokenKind::HAS_REF;
    if (w == "alive")     return QueryTokenKind::ALIVE;
    if (w == "any")       return QueryTokenKind::ANY;

    // Case-insensitive boolean operators
    std::string upper;
    upper.reserve(w.size());
    for (char c : w) upper += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (upper == "AND") return QueryTokenKind::AND;
    if (upper == "OR")  return QueryTokenKind::OR;
    if (upper == "NOT") return QueryTokenKind::NOT;

    return QueryTokenKind::IDENT;
  }
};

// ===========================================================================
// Parser (private to this TU)
// ===========================================================================

class QueryParser {
 public:
  explicit QueryParser(std::vector<QueryToken> tokens)
      : tokens_(std::move(tokens)), cursor_(0) {}

  // Top-level entry: parse the full expression.
  Result<QueryExprPtr> parse() {
    auto res = parse_or_expr();
    if (!res) return res.status();
    // After the full expression we must be at END
    if (peek().kind != QueryTokenKind::END) {
      return fail(Code::BadValue,
                  "unexpected token at pos " +
                      std::to_string(peek().pos));
    }
    return res;
  }

 private:
  std::vector<QueryToken> tokens_;
  std::size_t cursor_;

  const QueryToken& peek() const {
    // tokens_ always has at least the END sentinel
    return tokens_[cursor_];
  }

  QueryToken consume() {
    QueryToken t = tokens_[cursor_];
    if (cursor_ + 1 < tokens_.size()) ++cursor_;
    return t;
  }

  // Advance if the current token matches `k`, otherwise return error.
  Status expect(QueryTokenKind k) {
    if (peek().kind != k) {
      return fail(Code::BadValue,
                  "expected token " + std::to_string(static_cast<int>(k)) +
                      " but got " +
                      std::to_string(static_cast<int>(peek().kind)) +
                      " at pos " + std::to_string(peek().pos));
    }
    consume();
    return Status::ok();
  }

  // expr := or_expr
  Result<QueryExprPtr> parse_or_expr() {
    auto lhs = parse_and_expr();
    if (!lhs) return lhs.status();

    while (peek().kind == QueryTokenKind::OR) {
      consume();  // eat OR
      auto rhs = parse_and_expr();
      if (!rhs) return rhs.status();

      auto node = QueryExpr::make(QueryNodeKind::Or);
      node->children.push_back(lhs.take());
      node->children.push_back(rhs.take());
      // Re-wrap as lhs for the next iteration
      lhs = Result<QueryExprPtr>(std::move(node));
    }
    return lhs;
  }

  Result<QueryExprPtr> parse_and_expr() {
    auto lhs = parse_not_expr();
    if (!lhs) return lhs.status();

    while (peek().kind == QueryTokenKind::AND) {
      consume();  // eat AND
      auto rhs = parse_not_expr();
      if (!rhs) return rhs.status();

      auto node = QueryExpr::make(QueryNodeKind::And);
      node->children.push_back(lhs.take());
      node->children.push_back(rhs.take());
      lhs = Result<QueryExprPtr>(std::move(node));
    }
    return lhs;
  }

  Result<QueryExprPtr> parse_not_expr() {
    if (peek().kind == QueryTokenKind::NOT) {
      consume();  // eat NOT
      auto inner = parse_not_expr();
      if (!inner) return inner.status();

      auto node = QueryExpr::make(QueryNodeKind::Not);
      node->children.push_back(inner.take());
      return Result<QueryExprPtr>(std::move(node));
    }
    return parse_atom_expr();
  }

  Result<QueryExprPtr> parse_atom_expr() {
    QueryTokenKind k = peek().kind;

    // '(' expr ')'
    if (k == QueryTokenKind::LPAREN) {
      consume();
      auto inner = parse_or_expr();
      if (!inner) return inner.status();
      Status s = expect(QueryTokenKind::RPAREN);
      if (s.is_error()) return s;
      return inner;
    }

    // 'alive'
    if (k == QueryTokenKind::ALIVE) {
      consume();
      return Result<QueryExprPtr>(QueryExpr::make(QueryNodeKind::AnyAlive));
    }

    // 'any'
    if (k == QueryTokenKind::ANY) {
      consume();
      return Result<QueryExprPtr>(QueryExpr::make(QueryNodeKind::AnyAlive));
    }

    // 'kind' ':' ident
    if (k == QueryTokenKind::KIND) {
      consume();
      Status s = expect(QueryTokenKind::COLON);
      if (s.is_error()) return s;
      if (peek().kind != QueryTokenKind::IDENT) {
        return fail(Code::BadValue,
                    "expected identifier after 'kind:' at pos " +
                        std::to_string(peek().pos));
      }
      QueryToken name_tok = consume();
      auto node = QueryExpr::make(QueryNodeKind::KindMatch);
      node->atom_name = std::move(name_tok.string_val);
      return Result<QueryExprPtr>(std::move(node));
    }

    // 'has_child' ':' ident
    if (k == QueryTokenKind::HAS_CHILD) {
      consume();
      Status s = expect(QueryTokenKind::COLON);
      if (s.is_error()) return s;
      if (peek().kind != QueryTokenKind::IDENT) {
        return fail(Code::BadValue,
                    "expected identifier after 'has_child:' at pos " +
                        std::to_string(peek().pos));
      }
      QueryToken name_tok = consume();
      auto node = QueryExpr::make(QueryNodeKind::HasChild);
      node->atom_name = std::move(name_tok.string_val);
      return Result<QueryExprPtr>(std::move(node));
    }

    // 'has_ref' ':' ident
    if (k == QueryTokenKind::HAS_REF) {
      consume();
      Status s = expect(QueryTokenKind::COLON);
      if (s.is_error()) return s;
      if (peek().kind != QueryTokenKind::IDENT) {
        return fail(Code::BadValue,
                    "expected identifier after 'has_ref:' at pos " +
                        std::to_string(peek().pos));
      }
      QueryToken name_tok = consume();
      auto node = QueryExpr::make(QueryNodeKind::HasRef);
      node->atom_name = std::move(name_tok.string_val);
      return Result<QueryExprPtr>(std::move(node));
    }

    // 'field' ':' ident cmp_op value
    if (k == QueryTokenKind::FIELD) {
      consume();
      Status s = expect(QueryTokenKind::COLON);
      if (s.is_error()) return s;
      if (peek().kind != QueryTokenKind::IDENT) {
        return fail(Code::BadValue,
                    "expected field name after 'field:' at pos " +
                        std::to_string(peek().pos));
      }
      QueryToken field_tok = consume();

      // Parse comparison operator
      CmpOp op;
      switch (peek().kind) {
        case QueryTokenKind::EQ:  op = CmpOp::Eq;  break;
        case QueryTokenKind::NEQ: op = CmpOp::Neq; break;
        case QueryTokenKind::LT:  op = CmpOp::Lt;  break;
        case QueryTokenKind::GT:  op = CmpOp::Gt;  break;
        case QueryTokenKind::LE:  op = CmpOp::Le;  break;
        case QueryTokenKind::GE:  op = CmpOp::Ge;  break;
        default:
          return fail(Code::BadValue,
                      "expected comparison operator after field name at pos " +
                          std::to_string(peek().pos));
      }
      consume();  // eat the operator token

      // Parse value: STRING or INTEGER
      auto node = QueryExpr::make(QueryNodeKind::FieldCmp);
      node->atom_name = std::move(field_tok.string_val);
      node->cmp_op = op;

      if (peek().kind == QueryTokenKind::STRING) {
        QueryToken val_tok = consume();
        node->atom_val = std::move(val_tok.string_val);
        node->use_atom_val = true;
      } else if (peek().kind == QueryTokenKind::INTEGER) {
        QueryToken val_tok = consume();
        node->int_val = val_tok.int_val;
        node->use_atom_val = false;
      } else {
        return fail(Code::BadValue,
                    "expected string or integer value at pos " +
                        std::to_string(peek().pos));
      }
      return Result<QueryExprPtr>(std::move(node));
    }

    return fail(Code::BadValue,
                "unexpected token at pos " + std::to_string(peek().pos));
  }
};

// ===========================================================================
// QueryEngine constructor
// ===========================================================================

QueryEngine::QueryEngine(const AtomPool& atoms,
                         const Schema& schema,
                         const std::vector<Node>& nodes)
    : atoms_(atoms), schema_(schema), nodes_(nodes) {}

// ===========================================================================
// QueryEngine::parse()
// ===========================================================================

Result<QueryExprPtr> QueryEngine::parse(const std::string& query_text) {
  QueryLexer lexer(query_text);
  std::vector<QueryToken> tokens;
  Status lex_status = lexer.tokenize(tokens);
  if (lex_status.is_error()) return lex_status;

  QueryParser parser(std::move(tokens));
  return parser.parse();
}

}  // namespace stratavm
