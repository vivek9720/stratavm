// query_eval.cpp - query expression evaluator for the Stratavm node graph.
//
// Implements QueryEngine::execute() and the per-node predicate logic.
//
// Evaluation strategy:
//   - Iterate over every slot in nodes_ (including dead nodes for scanned count).
//   - Skip nodes where alive == false for all predicates except NOT (a NOT query
//     over a dead node still returns false because we skip dead nodes in the
//     top-level scan).
//   - For KindMatch: resolve the query kind name string to an AtomId, then
//     compare node.kind via schema_.kind(node.kind)->name.
//   - For FieldCmp: find the FieldId by scanning the kind's FieldDef list for a
//     field whose name atom matches the query field name; read the value from
//     node.fields; compare using the CmpOp.
//   - For HasChild: check whether any NodeId in node.children references a node
//     whose name atom matches the query name.
//   - For HasRef: same logic but over node.refs.
//   - And / Or / Not use standard boolean short-circuit evaluation.
//   - AnyAlive: always true (the outer scan already requires alive == true).

#include "stratavm/query.hpp"
#include "stratavm/atom_pool.hpp"
#include "stratavm/schema.hpp"
#include "stratavm/node.hpp"
#include "stratavm/status.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace stratavm {

// ---------------------------------------------------------------------------
// apply_cmp — compare two int64 values with the given operator
// ---------------------------------------------------------------------------

/*static*/
bool QueryEngine::apply_cmp(CmpOp op, std::int64_t lhs, std::int64_t rhs) {
  switch (op) {
    case CmpOp::Eq:  return lhs == rhs;
    case CmpOp::Neq: return lhs != rhs;
    case CmpOp::Lt:  return lhs <  rhs;
    case CmpOp::Gt:  return lhs >  rhs;
    case CmpOp::Le:  return lhs <= rhs;
    case CmpOp::Ge:  return lhs >= rhs;
  }
  return false;
}

// ---------------------------------------------------------------------------
// eval_node — recursive predicate evaluation for a single node
// ---------------------------------------------------------------------------

bool QueryEngine::eval_node(const QueryExpr& expr, const Node& node) const {
  switch (expr.kind) {
    // -----------------------------------------------------------------------
    case QueryNodeKind::True:
      return true;

    // -----------------------------------------------------------------------
    case QueryNodeKind::AnyAlive:
      // The outer loop already filters to alive nodes; just say yes.
      return true;

    // -----------------------------------------------------------------------
    case QueryNodeKind::And:
      // Short-circuit: if either child is false, result is false.
      if (expr.children.size() < 2) return false;
      return eval_node(*expr.children[0], node) &&
             eval_node(*expr.children[1], node);

    // -----------------------------------------------------------------------
    case QueryNodeKind::Or:
      if (expr.children.size() < 2) return false;
      return eval_node(*expr.children[0], node) ||
             eval_node(*expr.children[1], node);

    // -----------------------------------------------------------------------
    case QueryNodeKind::Not:
      if (expr.children.empty()) return false;
      return !eval_node(*expr.children[0], node);

    // -----------------------------------------------------------------------
    case QueryNodeKind::KindMatch: {
      // Resolve the query kind name to an AtomId.
      AtomId query_atom = atoms_.find(expr.atom_name);
      if (query_atom == kInvalidAtom) return false;

      // Get the KindDef for this node and compare its name atom.
      if (node.kind == kInvalidKind) return false;
      const KindDef* kdef = schema_.kind(node.kind);
      if (!kdef) return false;
      return kdef->name == query_atom;
    }

    // -----------------------------------------------------------------------
    case QueryNodeKind::FieldCmp: {
      // We need: the field id for `expr.atom_name` within this node's kind.
      if (node.kind == kInvalidKind) return false;
      const KindDef* kdef = schema_.kind(node.kind);
      if (!kdef) return false;

      // Resolve field name to AtomId.
      AtomId field_name_atom = atoms_.find(expr.atom_name);
      if (field_name_atom == kInvalidAtom) return false;

      // Find the FieldId by scanning the kind's field list.
      // 0xFFFFFFFF is our not-found sentinel (same value as kInvalidKind/kInvalidAtom).
      constexpr FieldId kNotFound = 0xFFFFFFFFu;
      FieldId fid = kNotFound;
      for (FieldId i = 0; i < static_cast<FieldId>(kdef->fields.size()); ++i) {
        if (kdef->fields[i].name == field_name_atom) {
          fid = i;
          break;
        }
      }
      if (fid == kNotFound) return false;

      // Read the value from node.fields.
      const Value* val = node.field(fid);
      if (!val) return false;

      if (expr.use_atom_val) {
        // String comparison: only meaningful for Atom-typed fields.
        // We compare by looking up both atoms and comparing their ids, OR
        // by resolving the stored atom id to its text.
        if (val->type() == ValueType::Atom) {
          AtomId stored_atom = val->as_atom();
          AtomId query_atom  = atoms_.find(expr.atom_val);
          if (query_atom == kInvalidAtom) return false;
          // Eq/Neq are meaningful; Lt/Gt/Le/Ge compare atom ids (numeric order).
          return apply_cmp(expr.cmp_op,
                           static_cast<std::int64_t>(stored_atom),
                           static_cast<std::int64_t>(query_atom));
        }
        // Not an atom field — cannot compare to a string literal.
        return false;
      } else {
        // Integer comparison
        std::int64_t stored = 0;
        switch (val->type()) {
          case ValueType::Int:
            stored = val->as_int();
            break;
          case ValueType::Ref:
            stored = static_cast<std::int64_t>(val->as_ref());
            break;
          case ValueType::Atom:
            stored = static_cast<std::int64_t>(val->as_atom());
            break;
          default:
            return false;
        }
        return apply_cmp(expr.cmp_op, stored, expr.int_val);
      }
    }

    // -----------------------------------------------------------------------
    case QueryNodeKind::HasChild: {
      // Look for a child node whose name atom matches expr.atom_name.
      AtomId query_atom = atoms_.find(expr.atom_name);
      if (query_atom == kInvalidAtom) return false;

      for (NodeId child_id : node.children) {
        if (child_id >= nodes_.size()) continue;
        const Node& child = nodes_[child_id];
        if (!child.alive) continue;
        if (child.name == query_atom) return true;
      }
      return false;
    }

    // -----------------------------------------------------------------------
    case QueryNodeKind::HasRef: {
      // Look for a referenced node whose name atom matches expr.atom_name.
      AtomId query_atom = atoms_.find(expr.atom_name);
      if (query_atom == kInvalidAtom) return false;

      for (NodeId ref_id : node.refs) {
        if (ref_id >= nodes_.size()) continue;
        const Node& ref_node = nodes_[ref_id];
        if (!ref_node.alive) continue;
        if (ref_node.name == query_atom) return true;
      }
      return false;
    }

    // -----------------------------------------------------------------------
    case QueryNodeKind::Literal:
      // Not produced by the parser; treat as false.
      return false;
  }

  return false;
}

// ---------------------------------------------------------------------------
// execute(expr) — main evaluation loop
// ---------------------------------------------------------------------------

QueryResult QueryEngine::execute(const QueryExprPtr& expr) {
  QueryResult result;
  if (!expr) return result;

  for (const Node& node : nodes_) {
    ++result.scanned;

    // Skip dead nodes in the top-level scan.
    if (!node.alive) continue;

    if (eval_node(*expr, node)) {
      ++result.matched;
      result.nodes.push_back(node.id);
    }
  }
  return result;
}

// ---------------------------------------------------------------------------
// execute(string) — convenience parse+execute
// ---------------------------------------------------------------------------

QueryResult QueryEngine::execute(const std::string& query_text) {
  auto parse_result = parse(query_text);
  if (!parse_result) {
    // Return empty result on parse failure.
    return QueryResult{};
  }
  return execute(parse_result.value());
}

}  // namespace stratavm
