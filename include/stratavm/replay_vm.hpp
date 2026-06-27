// replay_vm.hpp - the stateful machine that reconstructs the object graph.
//
// The VM is seeded with the initial heap, then it streams the journal opcode by
// opcode, decoding operands inline and mutating its node table. The interesting
// state is the *group construction stack*: BEGIN_GROUP opens a frame bound to an
// owner node, ADD_CHILD queues child names onto the open frame, and END_GROUP
// resolves those queued names - creating placeholder nodes for any forward
// references - and wires them onto the owner.
//
// Nodes are stored contiguously so the graph stays cache-friendly and ids are
// just indices. The group frame keeps both the owner's id and a cached pointer
// to its slot; the cached pointer is the fast path used while resolving a
// frame's deferred children.
#ifndef STRATAVM_REPLAY_VM_HPP
#define STRATAVM_REPLAY_VM_HPP

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "stratavm/atom_pool.hpp"
#include "stratavm/byte_reader.hpp"
#include "stratavm/heap.hpp"
#include "stratavm/node.hpp"
#include "stratavm/schema.hpp"
#include "stratavm/status.hpp"

namespace stratavm {

// A point-in-time marker recorded by SNAPSHOT. Snapshots are lightweight: they
// remember the label and how many nodes existed when taken, which is enough for
// the validator to reason about monotonic growth between markers.
struct Snapshot {
  AtomId label = kInvalidAtom;
  std::uint32_t node_count = 0;
  std::uint32_t alive_count = 0;
};

// One open group on the construction stack.
struct GroupFrame {
  NodeId owner_id = kInvalidNode;
  Node* owner_cache = nullptr;       // cached slot pointer, valid at open time
  AtomId owner_name = kInvalidAtom;
  std::uint32_t reserve_hint = 0;    // producer's estimate of children to add
  std::vector<AtomId> deferred_children;
};

// Aggregate counters surfaced to the CLI and tests after a run.
struct ReplayStats {
  std::uint32_t ops_executed = 0;
  std::uint32_t nodes_created = 0;
  std::uint32_t placeholders = 0;
  std::uint32_t nodes_dropped = 0;
  std::uint32_t children_linked = 0;
  std::uint32_t unresolved_refs = 0;
  std::uint32_t snapshots = 0;
};

class ReplayVM {
 public:
  ReplayVM(const Schema& schema, const AtomPool& atoms, const Limits& limits)
      : schema_(schema), atoms_(atoms), limits_(limits) {}

  // Install the decoded initial heap. Builds the name index for named nodes.
  void seed(HeapImage&& heap);

  // Decode-and-execute the JRNL payload (varint op_count followed by ops).
  Status run(ByteReader& journal);

  // Accessors used by the validator, the cross-ref index builder and the CLI.
  const std::vector<Node>& nodes() const { return nodes_; }
  std::size_t node_count() const { return nodes_.size(); }
  const ReplayStats& stats() const { return stats_; }
  const std::vector<Snapshot>& snapshots() const { return snapshots_; }
  const std::unordered_map<AtomId, AtomId>& meta() const { return meta_; }

  NodeId find_named(AtomId name) const {
    auto it = name_index_.find(name);
    return it == name_index_.end() ? kInvalidNode : it->second;
  }

 private:
  // Opcode handlers. Each reads its own operands from `r`.
  Status do_new_node(ByteReader& r);
  Status do_set_field(ByteReader& r);
  Status do_begin_group(ByteReader& r);
  Status do_end_group();
  Status do_add_child(ByteReader& r);
  Status do_link_ref(ByteReader& r);
  Status do_drop_node(ByteReader& r);
  Status do_snapshot(ByteReader& r);
  Status do_set_meta(ByteReader& r);

  // Resolve a frame's queued child names, materialising placeholders for any
  // that are not yet defined, and wire them onto the owner.
  Status resolve_frame(GroupFrame& frame);

  // Create a not-yet-defined node so a forward reference has something to point
  // at. Registers the name so later definitions can find the same slot.
  NodeId materialize_placeholder(AtomId name);

  void register_name(AtomId name, NodeId id);

  const Schema& schema_;
  const AtomPool& atoms_;
  const Limits& limits_;

  std::vector<Node> nodes_;
  std::unordered_map<AtomId, NodeId> name_index_;
  std::vector<GroupFrame> frames_;
  std::vector<NodeId> free_list_;
  std::vector<Snapshot> snapshots_;
  std::unordered_map<AtomId, AtomId> meta_;
  ReplayStats stats_;
};

}  // namespace stratavm

#endif  // STRATAVM_REPLAY_VM_HPP
