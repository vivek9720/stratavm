#include "stratavm/heap.hpp"

namespace stratavm {

Status decode_heap(ByteReader& r, const Schema& schema, const AtomPool& atoms,
                   const Limits& limits, HeapImage* out) {
  std::uint32_t node_count = 0;
  STRATAVM_RETURN_IF_ERROR(r.read_varint_u32(&node_count));
  if (node_count > limits.max_nodes)
    return fail(Code::LimitExceeded, "heap node count too large");

  out->nodes.clear();
  out->nodes.reserve(node_count);

  for (std::uint32_t i = 0; i < node_count; ++i) {
    Node node;

    std::uint32_t on_disk_id = 0;
    STRATAVM_RETURN_IF_ERROR(r.read_varint_u32(&on_disk_id));
    // Ids are positional. Enforcing this means a Ref value can be range-checked
    // against the node count without a second resolution pass.
    if (on_disk_id != i)
      return fail(Code::BadHeap, "non-sequential heap node id");
    node.id = i;

    STRATAVM_RETURN_IF_ERROR(r.read_varint_u32(&node.kind));
    const KindDef* kind = schema.kind(node.kind);
    if (kind == nullptr)
      return fail(Code::KindOutOfRange, "heap node kind out of range");

    STRATAVM_RETURN_IF_ERROR(r.read_varint_u32(&node.name));
    // A name of kInvalidAtom (encoded as the max varint) means "anonymous".
    if (node.name != kInvalidAtom && atoms.get(node.name) == nullptr)
      return fail(Code::AtomOutOfRange, "heap node name out of range");

    std::uint32_t field_count = 0;
    STRATAVM_RETURN_IF_ERROR(r.read_varint_u32(&field_count));
    if (field_count > kind->fields.size())
      return fail(Code::BadHeap, "more fields than the kind declares");

    for (std::uint32_t f = 0; f < field_count; ++f) {
      std::uint32_t field_idx = 0;
      STRATAVM_RETURN_IF_ERROR(r.read_varint_u32(&field_idx));
      const FieldDef* fd = kind->field(field_idx);
      if (fd == nullptr)
        return fail(Code::FieldOutOfRange, "heap field index out of range");

      Value v;
      Status st = v.decode(r, fd->type, limits);
      if (st.is_error()) return fail(Code::BadHeap, "field value: " + st.detail());
      node.set_field(field_idx, std::move(v));
    }
    out->nodes.push_back(std::move(node));
  }
  return Status::ok();
}

}  // namespace stratavm
