#include "stratavm/xref_index.hpp"

namespace stratavm {

void XrefIndex::build(const std::vector<Node>& nodes) {
  name_to_id_.clear();
  kind_to_ids_.clear();
  reverse_adj_.assign(nodes.size(), {});

  for (const Node& n : nodes) {
    if (n.name != kInvalidAtom && n.alive)
      name_to_id_[n.name] = n.id;
    if (n.kind != kInvalidKind)
      kind_to_ids_[n.kind].push_back(n.id);
  }

  // Reverse adjacency: for every outgoing ref, record the source under the
  // target. Targets are bounded by the table size because the VM only records
  // in-range refs.
  for (const Node& n : nodes) {
    for (NodeId t : n.refs) {
      if (t < reverse_adj_.size())
        reverse_adj_[t].push_back(n.id);
    }
    for (NodeId c : n.children) {
      if (c < reverse_adj_.size())
        reverse_adj_[c].push_back(n.id);
    }
  }
}

Status XrefIndex::decode_and_reconcile(ByteReader& r, const AtomPool& atoms,
                                       const Limits& limits, std::size_t node_count,
                                       std::uint32_t* reconcile_warnings) {
  std::uint32_t entry_count = 0;
  STRATAVM_RETURN_IF_ERROR(r.read_varint_u32(&entry_count));
  if (entry_count > limits.max_nodes)
    return fail(Code::LimitExceeded, "xref entry count too large");

  std::uint32_t warnings = 0;
  for (std::uint32_t i = 0; i < entry_count; ++i) {
    XrefEntry e;
    STRATAVM_RETURN_IF_ERROR(r.read_varint_u32(&e.name));
    STRATAVM_RETURN_IF_ERROR(r.read_varint_u32(&e.node));

    // Structural validation: names and node ids must be in range. These are
    // hard errors because they indicate a corrupt directory, not a stale index.
    if (e.name != kInvalidAtom && atoms.get(e.name) == nullptr)
      return fail(Code::BadXref, "xref name out of range");
    if (e.node != kInvalidNode && e.node >= node_count)
      return fail(Code::BadXref, "xref node out of range");

    // Soft reconciliation: if the precomputed mapping disagrees with what we
    // actually reconstructed, count it but keep going.
    NodeId actual = by_name(e.name);
    if (actual != e.node) warnings++;
  }
  if (reconcile_warnings) *reconcile_warnings += warnings;
  return Status::ok();
}

}  // namespace stratavm
