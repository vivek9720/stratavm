// field_index.hpp - secondary index over node field values for fast lookup.
//
// After the replay VM has finished reconstructing the node graph, a FieldIndex
// can be built in a single pass to allow efficient queries of the form "which
// alive nodes of kind K have field F equal to value V?". The index is
// intentionally read-only after build() returns; it must be rebuilt if the
// node graph changes.
#ifndef STRATAVM_FIELD_INDEX_HPP
#define STRATAVM_FIELD_INDEX_HPP

#include <cstdint>
#include <functional>
#include <map>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "stratavm/atom_pool.hpp"
#include "stratavm/format.hpp"
#include "stratavm/node.hpp"
#include "stratavm/schema.hpp"
#include "stratavm/value.hpp"

namespace stratavm {

// An entry describing which node has a particular (field_idx, value) pair.
// Stored as the value type in internal maps; exposed via for_each_in_kind.
struct FieldIndexEntry {
  NodeId  node_id;
  FieldId field_idx;
  KindId  kind;
};

// FieldIndex: built from a fully-replayed node graph. Supports lookup by
// (kind, field_idx, value) and by (field_idx, atom_value) across all kinds.
//
// The index only covers alive nodes. Dead (dropped) nodes are not indexed.
// Rebuild after any mutation that changes liveness or field values.
class FieldIndex {
 public:
  FieldIndex() = default;

  // Build the index from the node table and schema. Only alive, non-placeholder
  // nodes are indexed. `atoms` is currently unused but accepted so callers can
  // pass the pool consistently with other index builders.
  void build(const std::vector<Node>& nodes, const Schema& schema,
             const AtomPool& atoms);

  // Find all alive nodes where field `field_idx` of kind `kind` equals
  // `int_val`. Returns an empty vector when no match exists.
  std::vector<NodeId> lookup_int(KindId kind, FieldId field_idx,
                                  int64_t int_val) const;

  // Find all alive nodes where field `field_idx` of kind `kind` equals
  // `atom_val` (as an AtomId). Returns an empty vector when no match exists.
  std::vector<NodeId> lookup_atom(KindId kind, FieldId field_idx,
                                   AtomId atom_val) const;

  // Find all alive nodes of `kind` where field `field_idx` is set at all,
  // regardless of the value. Returns an empty vector when none exist.
  std::vector<NodeId> lookup_has_field(KindId kind, FieldId field_idx) const;

  // Find all nodes whose name atom matches `name`. Typically returns a single
  // node but placeholders may share a name atom before they are fully resolved.
  std::vector<NodeId> lookup_by_name(AtomId name) const;

  // Iterate all indexed entries for a given kind. Calls
  //   fn(NodeId, FieldId, const Value&)
  // for every (node, field, value) triple that was indexed during build().
  // The value reference is valid only for the duration of the callback.
  void for_each_in_kind(KindId kind,
                        std::function<void(NodeId, FieldId, const Value&)> fn) const;

  // Diagnostic accessors.
  size_t entry_count() const { return entry_count_; }
  size_t node_count()  const { return node_count_; }

  // Reset all internal state to the post-default-construction state.
  void clear();

 private:
  // Primary index: (kind, field_idx, int_val) → sorted list of NodeId.
  // Covers ValueType::Int fields (and ValueType::Float is not indexed by
  // value since exact float equality is rarely useful).
  std::map<std::tuple<KindId, FieldId, int64_t>, std::vector<NodeId>>
      int_index_;

  // Atom index: (kind, field_idx, atom_id) → sorted list of NodeId.
  // Covers ValueType::Atom fields.
  std::map<std::tuple<KindId, FieldId, AtomId>, std::vector<NodeId>>
      atom_index_;

  // Has-field index: (kind, field_idx) → list of NodeId.
  // Populated for every non-Invalid field regardless of value type.
  std::map<std::pair<KindId, FieldId>, std::vector<NodeId>>
      has_field_index_;

  // Name index: atom_id → list of NodeId (usually just one entry).
  std::unordered_map<AtomId, std::vector<NodeId>> name_index_;

  // Parallel storage of values so for_each_in_kind can return const Value&.
  // Keyed the same way as int_index_ / atom_index_ but stores the raw Value.
  // We keep a flat per-kind list: kind_id → vector of (FieldId, Value, NodeId).
  struct KindEntry {
    NodeId  node_id;
    FieldId field_idx;
    Value   value;
  };
  std::unordered_map<KindId, std::vector<KindEntry>> kind_entries_;

  size_t entry_count_ = 0;
  size_t node_count_  = 0;
};

}  // namespace stratavm

#endif  // STRATAVM_FIELD_INDEX_HPP
