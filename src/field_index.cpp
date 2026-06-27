// field_index.cpp - implementation of FieldIndex.
//
// The index is built in O(N*F) where N is the number of alive nodes and F is
// the average number of fields per node. Lookups are O(log M) where M is the
// number of distinct (kind, field, value) triples. This is acceptable for the
// post-replay analysis use-case; the index is not intended for hot paths inside
// the VM itself.
#include "stratavm/field_index.hpp"

#include <algorithm>
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

// ---------------------------------------------------------------------------
// build
// ---------------------------------------------------------------------------

void FieldIndex::build(const std::vector<Node>& nodes, const Schema& schema,
                       const AtomPool& /*atoms*/) {
  clear();

  for (const Node& n : nodes) {
    // Only index alive, fully-defined nodes.
    if (!n.alive || n.placeholder) continue;

    ++node_count_;

    // Index the node's name atom if it has one.
    if (n.name != kInvalidAtom) {
      name_index_[n.name].push_back(n.id);
    }

    // Walk every field stored on the node.
    for (const auto& [field_idx, val] : n.fields) {
      if (!val.valid()) continue;

      // has-field index: always populated for any valid field.
      has_field_index_[{n.kind, field_idx}].push_back(n.id);

      // Value-type-specific secondary indices.
      switch (val.type()) {
        case ValueType::Int:
          int_index_[{n.kind, field_idx, val.as_int()}].push_back(n.id);
          break;
        case ValueType::Atom:
          atom_index_[{n.kind, field_idx, val.as_atom()}].push_back(n.id);
          break;
        default:
          // Float, Ref, List are recorded in has_field_index_ but not in
          // value-keyed lookup structures (exact-equality queries on these
          // types are not supported by the current API).
          break;
      }

      // Keep a per-kind flat list for for_each_in_kind().
      kind_entries_[n.kind].push_back({n.id, field_idx, val});

      ++entry_count_;
    }
  }
}

// ---------------------------------------------------------------------------
// lookup_int
// ---------------------------------------------------------------------------

std::vector<NodeId> FieldIndex::lookup_int(KindId kind, FieldId field_idx,
                                            int64_t int_val) const {
  auto it = int_index_.find({kind, field_idx, int_val});
  if (it == int_index_.end()) return {};
  return it->second;
}

// ---------------------------------------------------------------------------
// lookup_atom
// ---------------------------------------------------------------------------

std::vector<NodeId> FieldIndex::lookup_atom(KindId kind, FieldId field_idx,
                                             AtomId atom_val) const {
  auto it = atom_index_.find({kind, field_idx, atom_val});
  if (it == atom_index_.end()) return {};
  return it->second;
}

// ---------------------------------------------------------------------------
// lookup_has_field
// ---------------------------------------------------------------------------

std::vector<NodeId> FieldIndex::lookup_has_field(KindId kind,
                                                  FieldId field_idx) const {
  auto it = has_field_index_.find({kind, field_idx});
  if (it == has_field_index_.end()) return {};
  return it->second;
}

// ---------------------------------------------------------------------------
// lookup_by_name
// ---------------------------------------------------------------------------

std::vector<NodeId> FieldIndex::lookup_by_name(AtomId name) const {
  auto it = name_index_.find(name);
  if (it == name_index_.end()) return {};
  return it->second;
}

// ---------------------------------------------------------------------------
// for_each_in_kind
// ---------------------------------------------------------------------------

void FieldIndex::for_each_in_kind(
    KindId kind,
    std::function<void(NodeId, FieldId, const Value&)> fn) const {
  auto it = kind_entries_.find(kind);
  if (it == kind_entries_.end()) return;
  for (const KindEntry& e : it->second) {
    fn(e.node_id, e.field_idx, e.value);
  }
}

// ---------------------------------------------------------------------------
// clear
// ---------------------------------------------------------------------------

void FieldIndex::clear() {
  int_index_.clear();
  atom_index_.clear();
  has_field_index_.clear();
  name_index_.clear();
  kind_entries_.clear();
  entry_count_ = 0;
  node_count_  = 0;
}

}  // namespace stratavm
