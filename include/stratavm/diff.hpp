// diff.hpp - produce and apply diffs between two container node graphs.
//
// A GraphDiff records which named nodes were added, removed or modified when
// comparing two fully-replayed node tables. Nodes are matched by their name
// atom (AtomId): that is stable across snapshots because both the before- and
// after-states share the same atom pool.
//
// Three serialization forms are provided:
//   1. Binary (serialize_diff / deserialize_diff) - compact byte stream.
//   2. Journal patch (diff_to_journal) - a journal segment that, when replayed
//      on top of the `before` state, produces the `after` state.
#ifndef STRATAVM_DIFF_HPP
#define STRATAVM_DIFF_HPP

#include <cstdint>
#include <vector>

#include "stratavm/atom_pool.hpp"
#include "stratavm/byte_reader.hpp"
#include "stratavm/byte_writer.hpp"
#include "stratavm/format.hpp"
#include "stratavm/node.hpp"
#include "stratavm/schema.hpp"
#include "stratavm/status.hpp"
#include "stratavm/value.hpp"

namespace stratavm {

// How a named node changed between the before and after graphs.
enum class NodeChangeKind : std::uint8_t {
  Added = 0,      // present in after but not in before (by name atom)
  Removed = 1,    // present in before but not in after
  Modified = 2,   // present in both, with at least one field/child difference
  Unchanged = 3,  // present in both, identical
};

// A change to a single field within a node.
struct FieldChange {
  FieldId field_idx = 0;
  Value before;  // Invalid value means the field was not set in before
  Value after;   // Invalid value means the field was removed in after
};

// The complete set of changes for one named node.
struct NodeChange {
  NodeChangeKind change = NodeChangeKind::Unchanged;

  NodeId before_id = kInvalidNode;  // kInvalidNode when change == Added
  NodeId after_id = kInvalidNode;   // kInvalidNode when change == Removed

  AtomId name = kInvalidAtom;  // name atom used to match across snapshots

  // The kind of the node in the after-state (or before-state for Removed).
  // Needed by diff_to_journal to emit correct NewNode ops.
  KindId kind = kInvalidKind;

  std::vector<FieldChange> field_changes;

  // Child name atoms present in after but not before.
  std::vector<AtomId> children_added;

  // Child name atoms present in before but not after.
  std::vector<AtomId> children_removed;
};

// The top-level diff between two graph states.
struct GraphDiff {
  std::vector<NodeChange> changes;

  std::uint32_t nodes_added = 0;
  std::uint32_t nodes_removed = 0;
  std::uint32_t nodes_modified = 0;
  std::uint32_t total_field_changes = 0;

  bool empty() const { return changes.empty(); }
};

// ---------------------------------------------------------------------------
// Diff computation
// ---------------------------------------------------------------------------

// Compare two fully-replayed node tables using name atoms as identity keys.
// Only alive, named nodes are compared (nodes with kInvalidAtom name or
// alive==false are skipped).
// Both tables must use the same atom pool (name atoms are comparable as ints).
GraphDiff diff_node_graphs(const std::vector<Node>& before_nodes,
                           const std::vector<Node>& after_nodes,
                           const Schema& schema);

// ---------------------------------------------------------------------------
// Binary serialization
// ---------------------------------------------------------------------------

// Serialize a GraphDiff to a compact binary byte stream.
//
// Wire format:
//   varint  change_count
//   for each NodeChange:
//     u8      kind            (NodeChangeKind byte)
//     varint  name_atom
//     varint  before_id       (kInvalidNode encoded as 0xFFFFFFFF)
//     varint  after_id        (kInvalidNode encoded as 0xFFFFFFFF)
//     varint  kind_id         (kInvalidKind encoded as 0xFFFFFFFF)
//     varint  field_change_count
//     for each FieldChange:
//       varint  field_idx
//       value   before        (type byte + payload)
//       value   after
//     varint  children_added_count
//     varint  children_added[i]  (each: atom id)
//     varint  children_removed_count
//     varint  children_removed[i]
//
// Value encoding (type byte + payload):
//   0x00  Int    svarint
//   0x01  Float  f64 (8 bytes LE)
//   0x02  Atom   varint
//   0x03  Ref    varint
//   0x04  List   varint count, then count varints
//   0xFF  Invalid  (no payload; represents "not set")
std::vector<std::uint8_t> serialize_diff(const GraphDiff& diff,
                                          const Schema& schema);

// Deserialize a GraphDiff from the byte stream produced by serialize_diff.
Result<GraphDiff> deserialize_diff(ByteReader& r, const Schema& schema);

// ---------------------------------------------------------------------------
// Journal generation
// ---------------------------------------------------------------------------

// Produce a journal byte stream (starts with a varint op_count) that, when
// replayed on a VM in the `before` state, produces a graph matching the
// `after` state described by the diff.
//
// Operations emitted:
//   Removed nodes:  DropNode(before_id)
//   Added nodes:    NewNode(kind, name) + SetField for each field
//   Modified nodes: SetField for each changed field, DropNode+NewNode+SetField
//                   if the kind changed
//
// Returns an error if the diff refers to invalid IDs.
Result<std::vector<std::uint8_t>> diff_to_journal(const GraphDiff& diff,
                                                   const Schema& schema,
                                                   const AtomPool& atoms);

}  // namespace stratavm

#endif  // STRATAVM_DIFF_HPP
