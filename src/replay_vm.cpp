#include "stratavm/replay_vm.hpp"

#include "stratavm/format.hpp"
#include "stratavm/journal.hpp"

namespace stratavm {

void ReplayVM::register_name(AtomId name, NodeId id) {
  if (name == kInvalidAtom) return;  // anonymous nodes are not indexed
  name_index_[name] = id;
}

void ReplayVM::seed(HeapImage&& heap) {
  nodes_ = std::move(heap.nodes);
  name_index_.clear();
  for (const Node& n : nodes_) {
    if (n.name != kInvalidAtom) name_index_[n.name] = n.id;
  }
  stats_.nodes_created = static_cast<std::uint32_t>(nodes_.size());
}

Status ReplayVM::run(ByteReader& r) {
  std::uint32_t op_count = 0;
  STRATAVM_RETURN_IF_ERROR(r.read_varint_u32(&op_count));
  if (op_count > limits_.max_journal_ops)
    return fail(Code::LimitExceeded, "journal op count too large");

  for (std::uint32_t i = 0; i < op_count; ++i) {
    std::uint8_t raw = 0;
    STRATAVM_RETURN_IF_ERROR(r.read_u8(&raw));
    if (!is_valid_opcode(raw))
      return fail(Code::BadOpcode, "opcode " + std::to_string(raw));

    OpCode op = static_cast<OpCode>(raw);
    Status st;
    switch (op) {
      case OpCode::NewNode: st = do_new_node(r); break;
      case OpCode::SetField: st = do_set_field(r); break;
      case OpCode::BeginGroup: st = do_begin_group(r); break;
      case OpCode::EndGroup: st = do_end_group(); break;
      case OpCode::AddChild: st = do_add_child(r); break;
      case OpCode::LinkRef: st = do_link_ref(r); break;
      case OpCode::DropNode: st = do_drop_node(r); break;
      case OpCode::Snapshot: st = do_snapshot(r); break;
      case OpCode::SetMeta: st = do_set_meta(r); break;
      case OpCode::Nop: st = Status::ok(); break;
    }
    if (st.is_error()) return st;
    stats_.ops_executed++;
  }

  // A well-formed container balances every group. An imbalance is a structural
  // error rather than a crash.
  if (!frames_.empty())
    return fail(Code::GroupUnderflow, "journal ended with open group");
  return Status::ok();
}

Status ReplayVM::do_new_node(ByteReader& r) {
  std::uint32_t kind = 0;
  std::uint32_t name = 0;
  STRATAVM_RETURN_IF_ERROR(r.read_varint_u32(&kind));
  STRATAVM_RETURN_IF_ERROR(r.read_varint_u32(&name));

  if (schema_.kind(kind) == nullptr)
    return fail(Code::KindOutOfRange, "new_node kind");
  if (name != kInvalidAtom && atoms_.get(name) == nullptr)
    return fail(Code::AtomOutOfRange, "new_node name");
  if (nodes_.size() >= limits_.max_nodes)
    return fail(Code::LimitExceeded, "node table full");

  Node node;
  node.id = static_cast<NodeId>(nodes_.size());
  node.kind = kind;
  node.name = name;
  node.alive = true;
  nodes_.push_back(std::move(node));
  register_name(name, nodes_.back().id);
  stats_.nodes_created++;
  return Status::ok();
}

Status ReplayVM::do_set_field(ByteReader& r) {
  std::uint32_t node_id = 0;
  std::uint32_t field_idx = 0;
  STRATAVM_RETURN_IF_ERROR(r.read_varint_u32(&node_id));
  STRATAVM_RETURN_IF_ERROR(r.read_varint_u32(&field_idx));
  if (node_id >= nodes_.size())
    return fail(Code::NodeOutOfRange, "set_field node");

  // The value's wire type is dictated by the node's kind, so we have to know the
  // kind before we can read the operand bytes.
  KindId kind = nodes_[node_id].kind;
  ValueType vt = schema_.field_type(kind, field_idx);
  if (vt == ValueType::Invalid)
    return fail(Code::FieldOutOfRange, "set_field field");

  Value v;
  STRATAVM_RETURN_IF_ERROR(v.decode(r, vt, limits_));

  Node& node = nodes_[node_id];
  if (!node.alive)
    return fail(Code::InvariantViolation, "set_field on dropped node");

  // For reference-bearing fields, record the outgoing edges so the validator and
  // the cross-reference index can see them.
  if (vt == ValueType::Ref) {
    if (v.as_ref() != kInvalidNode && v.as_ref() < nodes_.size())
      node.refs.push_back(v.as_ref());
    else
      stats_.unresolved_refs++;
  } else if (vt == ValueType::List) {
    for (NodeId t : v.as_list()) {
      if (t < nodes_.size())
        node.refs.push_back(t);
      else
        stats_.unresolved_refs++;
    }
  }
  node.set_field(field_idx, std::move(v));
  return Status::ok();
}

Status ReplayVM::do_begin_group(ByteReader& r) {
  std::uint32_t owner_name = 0;
  std::uint32_t reserve_hint = 0;
  STRATAVM_RETURN_IF_ERROR(r.read_varint_u32(&owner_name));
  STRATAVM_RETURN_IF_ERROR(r.read_varint_u32(&reserve_hint));

  if (frames_.size() >= limits_.max_group_depth)
    return fail(Code::GroupOverflow, "group nesting too deep");

  // The owner must already exist; groups attach children to a live node.
  NodeId owner_id = find_named(owner_name);
  if (owner_id == kInvalidNode || owner_id >= nodes_.size())
    return fail(Code::UnresolvedRef, "begin_group owner not found");
  if (!nodes_[owner_id].alive)
    return fail(Code::InvariantViolation, "begin_group owner dropped");

  GroupFrame frame;
  frame.owner_id = owner_id;
  frame.owner_name = owner_name;
  frame.reserve_hint = reserve_hint;

  // Reserve room for the children the producer says it is about to add. With
  // enough headroom the node table will not reallocate while the group is open,
  // so caching the owner slot pointer here is a safe fast path.
  nodes_.reserve(nodes_.size() + reserve_hint);
  frame.owner_cache = &nodes_[owner_id];

  frames_.push_back(std::move(frame));
  return Status::ok();
}

Status ReplayVM::do_add_child(ByteReader& r) {
  std::uint32_t child_name = 0;
  STRATAVM_RETURN_IF_ERROR(r.read_varint_u32(&child_name));
  if (frames_.empty())
    return fail(Code::GroupUnderflow, "add_child outside group");
  if (child_name != kInvalidAtom && atoms_.get(child_name) == nullptr)
    return fail(Code::AtomOutOfRange, "add_child name");
  frames_.back().deferred_children.push_back(child_name);
  return Status::ok();
}

Status ReplayVM::do_end_group() {
  if (frames_.empty())
    return fail(Code::GroupUnderflow, "end_group without group");
  GroupFrame frame = std::move(frames_.back());
  frames_.pop_back();
  return resolve_frame(frame);
}

NodeId ReplayVM::materialize_placeholder(AtomId name) {
  NodeId id = static_cast<NodeId>(nodes_.size());
  Node node;
  node.id = id;
  node.kind = kInvalidKind;
  node.name = name;
  node.alive = true;
  node.placeholder = true;
  // Appending here can grow the node table past whatever capacity was reserved
  // when the enclosing group opened.
  nodes_.push_back(std::move(node));
  register_name(name, id);
  stats_.placeholders++;
  return id;
}

Status ReplayVM::resolve_frame(GroupFrame& frame) {
  // Fast path: use the owner slot pointer captured when the group opened instead
  // of re-indexing the node table on every child.
  Node* owner = frame.owner_cache;

  for (AtomId child_name : frame.deferred_children) {
    NodeId target = find_named(child_name);
    if (target == kInvalidNode) {
      // Forward reference: nothing defines this name yet, so stand up a
      // placeholder the later definition can fill in.
      target = materialize_placeholder(child_name);
    }
    owner->children.push_back(target);
    nodes_[target].refs.push_back(owner->id);
    stats_.children_linked++;
  }
  return Status::ok();
}

Status ReplayVM::do_link_ref(ByteReader& r) {
  std::uint32_t src = 0;
  std::uint32_t field_idx = 0;
  std::uint32_t target_atom = 0;
  STRATAVM_RETURN_IF_ERROR(r.read_varint_u32(&src));
  STRATAVM_RETURN_IF_ERROR(r.read_varint_u32(&field_idx));
  STRATAVM_RETURN_IF_ERROR(r.read_varint_u32(&target_atom));

  if (src >= nodes_.size())
    return fail(Code::NodeOutOfRange, "link_ref src");
  ValueType vt = schema_.field_type(nodes_[src].kind, field_idx);
  if (vt != ValueType::Ref)
    return fail(Code::TypeMismatch, "link_ref target field is not a ref");

  NodeId target = find_named(target_atom);
  if (target == kInvalidNode) {
    // Unlike ADD_CHILD, a dangling LINK_REF does not synthesise a placeholder;
    // it is simply counted and left for the validator to flag.
    stats_.unresolved_refs++;
    return Status::ok();
  }
  nodes_[src].set_field(field_idx, Value::make_ref(target));
  nodes_[src].refs.push_back(target);
  return Status::ok();
}

Status ReplayVM::do_drop_node(ByteReader& r) {
  std::uint32_t node_id = 0;
  STRATAVM_RETURN_IF_ERROR(r.read_varint_u32(&node_id));
  if (node_id >= nodes_.size())
    return fail(Code::NodeOutOfRange, "drop_node");
  Node& node = nodes_[node_id];
  if (!node.alive) return Status::ok();  // dropping twice is a no-op
  node.alive = false;
  node.generation++;
  free_list_.push_back(node_id);
  if (node.name != kInvalidAtom) {
    auto it = name_index_.find(node.name);
    if (it != name_index_.end() && it->second == node_id)
      name_index_.erase(it);
  }
  stats_.nodes_dropped++;
  return Status::ok();
}

Status ReplayVM::do_snapshot(ByteReader& r) {
  std::uint32_t label = 0;
  STRATAVM_RETURN_IF_ERROR(r.read_varint_u32(&label));
  if (label != kInvalidAtom && atoms_.get(label) == nullptr)
    return fail(Code::AtomOutOfRange, "snapshot label");
  std::uint32_t alive = 0;
  for (const Node& n : nodes_) alive += n.alive ? 1 : 0;
  Snapshot snap;
  snap.label = label;
  snap.node_count = static_cast<std::uint32_t>(nodes_.size());
  snap.alive_count = alive;
  snapshots_.push_back(snap);
  stats_.snapshots++;
  return Status::ok();
}

Status ReplayVM::do_set_meta(ByteReader& r) {
  std::uint32_t key = 0;
  std::uint32_t value = 0;
  STRATAVM_RETURN_IF_ERROR(r.read_varint_u32(&key));
  STRATAVM_RETURN_IF_ERROR(r.read_varint_u32(&value));
  if (atoms_.get(key) == nullptr)
    return fail(Code::AtomOutOfRange, "meta key");
  if (value != kInvalidAtom && atoms_.get(value) == nullptr)
    return fail(Code::AtomOutOfRange, "meta value");
  meta_[key] = value;
  return Status::ok();
}

}  // namespace stratavm
