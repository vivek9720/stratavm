// node.hpp - a single reconstructed object in the graph.
//
// Nodes live in a contiguous std::vector owned by the replay machine. Each node
// records its kind, an optional interned name, its scalar/compound field values
// keyed by field index, and an ordered child list. The `alive` flag and
// `generation` counter support DROP_NODE / reuse semantics: a dropped node keeps
// its slot but is logically dead, and the generation bumps so stale references
// can be detected by code that bothers to check it.
#ifndef STRATAVM_NODE_HPP
#define STRATAVM_NODE_HPP

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "stratavm/schema.hpp"
#include "stratavm/value.hpp"

namespace stratavm {

struct Node {
  NodeId id = kInvalidNode;
  KindId kind = kInvalidKind;
  AtomId name = kInvalidAtom;
  bool alive = true;
  bool placeholder = false;  // materialised for a forward reference, not yet defined
  std::uint32_t generation = 0;

  // Field values keyed by field index within the node's kind.
  std::unordered_map<FieldId, Value> fields;

  // Ordered children accumulated via ADD_CHILD inside a group.
  std::vector<NodeId> children;

  // Outgoing references collected from Ref/List fields and LINK_REF ops. Used by
  // the validator and the cross-reference index.
  std::vector<NodeId> refs;

  void set_field(FieldId f, Value v) { fields[f] = std::move(v); }

  const Value* field(FieldId f) const {
    auto it = fields.find(f);
    return it == fields.end() ? nullptr : &it->second;
  }
};

}  // namespace stratavm

#endif  // STRATAVM_NODE_HPP
