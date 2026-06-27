// query.hpp - read-only query engine for the Stratavm node graph.
//
// Provides a small expression language for filtering nodes by kind, field
// values, child membership, and ref membership. Queries are compiled from a
// text string into a QueryExpr tree and then evaluated against a snapshot of
// the node graph. The engine is entirely read-only: it never modifies the
// AtomPool, Schema, or Node vector it is given.
//
// Grammar (informal):
//   expr     := or_expr
//   or_expr  := and_expr ('OR' and_expr)*
//   and_expr := not_expr ('AND' not_expr)*
//   not_expr := 'NOT' not_expr | atom_expr
//   atom_expr := 'kind' ':' ident
//              | 'field' ':' ident cmp_op value
//              | 'has_child' ':' ident
//              | 'has_ref' ':' ident
//              | 'alive'
//              | 'any'
//              | '(' expr ')'
//   cmp_op   := '=' | '!=' | '<' | '>' | '<=' | '>='
//   value    := STRING | INTEGER
#ifndef STRATAVM_QUERY_HPP
#define STRATAVM_QUERY_HPP

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "stratavm/atom_pool.hpp"
#include "stratavm/node.hpp"
#include "stratavm/schema.hpp"
#include "stratavm/status.hpp"

namespace stratavm {

// ---------------------------------------------------------------------------
// Lexer token kinds
// ---------------------------------------------------------------------------

enum class QueryTokenKind {
  // keywords
  KIND,
  FIELD,
  HAS_CHILD,
  HAS_REF,
  ALIVE,
  ANY,
  AND,
  OR,
  NOT,
  // punctuation
  LPAREN,
  RPAREN,
  // comparison operators
  EQ,
  NEQ,
  LT,
  GT,
  LE,
  GE,
  // structural
  COLON,
  // literals / names
  STRING,
  INTEGER,
  IDENT,
  // sentinel
  END,
};

struct QueryToken {
  QueryTokenKind kind = QueryTokenKind::END;
  std::string string_val;   // populated for STRING / IDENT tokens
  std::int64_t int_val = 0; // populated for INTEGER tokens
  std::size_t pos = 0;      // byte offset in the source string
};

// ---------------------------------------------------------------------------
// AST node kinds
// ---------------------------------------------------------------------------

enum class QueryNodeKind {
  Literal,    // unused by the parser; reserved for future constant folding
  And,        // binary: children[0] AND children[1]
  Or,         // binary: children[0] OR  children[1]
  Not,        // unary:  NOT children[0]
  KindMatch,  // node.kind name == atom_name
  FieldCmp,   // node.fields[field named atom_name] cmp_op int_val/atom_val
  HasChild,   // any child node has name == atom_name
  HasRef,     // any ref node   has name == atom_name
  AnyAlive,   // true for any alive node (used by "any")
  True,       // unconditional true (used internally)
};

enum class CmpOp {
  Eq,   // =
  Neq,  // !=
  Lt,   // <
  Gt,   // >
  Le,   // <=
  Ge,   // >=
};

// ---------------------------------------------------------------------------
// Query expression tree
// ---------------------------------------------------------------------------

struct QueryExpr {
  QueryNodeKind kind = QueryNodeKind::True;

  // Comparison operator; relevant for FieldCmp nodes.
  CmpOp cmp_op = CmpOp::Eq;

  // Name used by KindMatch, FieldCmp, HasChild, HasRef (the thing being
  // matched against; e.g. the kind name or field name).
  std::string atom_name;

  // Integer literal value for FieldCmp with an INTEGER rhs.
  std::int64_t int_val = 0;

  // String/atom literal value for FieldCmp with a STRING rhs.
  std::string atom_val;

  // When atom_val is used, this flag distinguishes it from int_val.
  bool use_atom_val = false;

  // Child sub-expressions (And/Or: exactly 2; Not: exactly 1).
  std::vector<std::unique_ptr<QueryExpr>> children;

  // Factory helpers.
  static std::unique_ptr<QueryExpr> make(QueryNodeKind k) {
    auto e = std::make_unique<QueryExpr>();
    e->kind = k;
    return e;
  }
};

using QueryExprPtr = std::unique_ptr<QueryExpr>;

// ---------------------------------------------------------------------------
// Query execution result
// ---------------------------------------------------------------------------

struct QueryResult {
  std::vector<NodeId> nodes;   // ids of nodes that matched
  std::uint32_t scanned = 0;   // total nodes inspected (including dead ones)
  std::uint32_t matched = 0;   // nodes that passed the predicate
};

// ---------------------------------------------------------------------------
// Query engine
// ---------------------------------------------------------------------------

class QueryEngine {
 public:
  // The engine holds const references; the caller must keep the objects alive
  // for the lifetime of the engine.
  QueryEngine(const AtomPool& atoms,
              const Schema& schema,
              const std::vector<Node>& nodes);

  // Parse a query string into an expression tree.
  // Returns an error Status when the input is syntactically invalid.
  Result<QueryExprPtr> parse(const std::string& query_text);

  // Evaluate a pre-compiled expression against the node graph.
  QueryResult execute(const QueryExprPtr& expr);

  // Convenience: parse then execute. On parse error returns an empty result.
  QueryResult execute(const std::string& query_text);

 private:
  const AtomPool& atoms_;
  const Schema& schema_;
  const std::vector<Node>& nodes_;

  // Evaluate `expr` against a single node. Returns true if the node matches.
  bool eval_node(const QueryExpr& expr, const Node& node) const;

  // Compare two int64 values according to `op`.
  static bool apply_cmp(CmpOp op, std::int64_t lhs, std::int64_t rhs);
};

}  // namespace stratavm

#endif  // STRATAVM_QUERY_HPP
