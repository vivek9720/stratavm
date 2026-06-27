// xref_index.hpp - cross-reference index over the reconstructed graph.
//
// Two things live here. First, a builder that walks the final node table and
// derives lookup structures: name -> id, kind -> ids, and a reverse adjacency
// (which nodes reference a given node). Second, a decoder for the optional XREF
// section, which carries a producer-precomputed name index. When present we load
// it and reconcile it against the freshly built index; disagreements are counted
// as warnings rather than hard failures so a stale index cannot wedge a load.
#ifndef STRATAVM_XREF_INDEX_HPP
#define STRATAVM_XREF_INDEX_HPP

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "stratavm/atom_pool.hpp"
#include "stratavm/byte_reader.hpp"
#include "stratavm/node.hpp"
#include "stratavm/status.hpp"

namespace stratavm {

struct XrefEntry {
  AtomId name = kInvalidAtom;
  NodeId node = kInvalidNode;
};

class XrefIndex {
 public:
  XrefIndex() = default;

  // Build the derived indices from the reconstructed node table.
  void build(const std::vector<Node>& nodes);

  // Decode an optional XREF section and reconcile it with the built index.
  // `reconcile_warnings` is incremented for each entry that disagrees with the
  // reconstructed graph.
  Status decode_and_reconcile(ByteReader& r, const AtomPool& atoms,
                              const Limits& limits, std::size_t node_count,
                              std::uint32_t* reconcile_warnings);

  NodeId by_name(AtomId name) const {
    auto it = name_to_id_.find(name);
    return it == name_to_id_.end() ? kInvalidNode : it->second;
  }

  const std::vector<NodeId>& by_kind(KindId kind) const {
    static const std::vector<NodeId> kEmpty;
    auto it = kind_to_ids_.find(kind);
    return it == kind_to_ids_.end() ? kEmpty : it->second;
  }

  const std::vector<NodeId>& referrers(NodeId target) const {
    static const std::vector<NodeId> kEmpty;
    if (target >= reverse_adj_.size()) return kEmpty;
    return reverse_adj_[target];
  }

  std::size_t name_count() const { return name_to_id_.size(); }

 private:
  std::unordered_map<AtomId, NodeId> name_to_id_;
  std::unordered_map<KindId, std::vector<NodeId>> kind_to_ids_;
  std::vector<std::vector<NodeId>> reverse_adj_;
};

}  // namespace stratavm

#endif  // STRATAVM_XREF_INDEX_HPP
