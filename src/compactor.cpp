// compactor.cpp - implementation of JournalCompactor.
#include "stratavm/compactor.hpp"

#include <algorithm>
#include <map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "stratavm/atom_pool.hpp"
#include "stratavm/byte_reader.hpp"
#include "stratavm/byte_writer.hpp"
#include "stratavm/format.hpp"
#include "stratavm/journal.hpp"
#include "stratavm/schema.hpp"
#include "stratavm/status.hpp"
#include "stratavm/value.hpp"

namespace stratavm {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

ValueType JournalCompactor::field_type_for(
    std::uint32_t node_id, std::uint32_t field_idx,
    const std::vector<std::uint32_t>& node_kinds,
    const Schema& schema) const {
  if (node_id >= node_kinds.size()) return ValueType::Invalid;
  std::uint32_t kind_id = node_kinds[node_id];
  return schema.field_type(static_cast<KindId>(kind_id),
                           static_cast<FieldId>(field_idx));
}

// ---------------------------------------------------------------------------
// decode_op
// ---------------------------------------------------------------------------

Result<OpRecord> JournalCompactor::decode_op(
    ByteReader& r, const Schema& schema,
    std::vector<std::uint32_t>& node_kinds,
    std::vector<OpRecord>& group_stack) {
  OpRecord rec;

  std::uint8_t raw_op = 0;
  STRATAVM_RETURN_IF_ERROR(r.read_u8(&raw_op));

  if (!is_valid_opcode(raw_op)) {
    return fail(Code::BadOpcode, "unknown opcode in compact");
  }

  rec.op = static_cast<OpCode>(raw_op);

  switch (rec.op) {
    case OpCode::NewNode: {
      std::uint32_t kind = 0, name_atom = 0;
      STRATAVM_RETURN_IF_ERROR(r.read_varint_u32(&kind));
      STRATAVM_RETURN_IF_ERROR(r.read_varint_u32(&name_atom));
      rec.kind = kind;
      rec.name_atom = name_atom;

      // Assign a node_id: the next slot in node_kinds.
      rec.node_id = static_cast<std::uint32_t>(node_kinds.size());
      node_kinds.push_back(kind);
      break;
    }

    case OpCode::SetField: {
      std::uint32_t node_id = 0, field_idx = 0;
      STRATAVM_RETURN_IF_ERROR(r.read_varint_u32(&node_id));
      STRATAVM_RETURN_IF_ERROR(r.read_varint_u32(&field_idx));
      rec.node_id = node_id;
      rec.field_idx = field_idx;

      // Determine the type from the schema so we can decode the value.
      ValueType vtype = field_type_for(node_id, field_idx, node_kinds, schema);
      if (vtype == ValueType::Invalid) {
        // Unknown type - we still need to consume bytes. If the schema doesn't
        // know this field, treat as Int (fallback) so we can at least advance
        // the cursor; the field will be marked dead anyway if the node is dead.
        vtype = ValueType::Int;
      }

      Limits limits;
      Status s = rec.value.decode(r, vtype, limits);
      if (s.is_error()) return s;
      break;
    }

    case OpCode::BeginGroup: {
      std::uint32_t owner_name_atom = 0, reserve_hint = 0;
      STRATAVM_RETURN_IF_ERROR(r.read_varint_u32(&owner_name_atom));
      STRATAVM_RETURN_IF_ERROR(r.read_varint_u32(&reserve_hint));
      rec.name_atom = owner_name_atom;
      rec.reserve_hint = reserve_hint;
      // Push onto group stack; children will be collected into it.
      group_stack.push_back(rec);
      break;
    }

    case OpCode::EndGroup: {
      // No operands. Pop from group stack and merge children into this record.
      if (group_stack.empty()) {
        return fail(Code::GroupUnderflow, "EndGroup without BeginGroup");
      }
      OpRecord& top = group_stack.back();
      rec.name_atom = top.name_atom;
      rec.reserve_hint = top.reserve_hint;
      rec.group_children = std::move(top.group_children);
      group_stack.pop_back();
      break;
    }

    case OpCode::AddChild: {
      std::uint32_t child_name_atom = 0;
      STRATAVM_RETURN_IF_ERROR(r.read_varint_u32(&child_name_atom));
      rec.target_atom = child_name_atom;
      // Add to current group frame if one is active.
      if (!group_stack.empty()) {
        group_stack.back().group_children.push_back(child_name_atom);
      }
      break;
    }

    case OpCode::LinkRef: {
      std::uint32_t src_node_id = 0, field_idx = 0, target_atom = 0;
      STRATAVM_RETURN_IF_ERROR(r.read_varint_u32(&src_node_id));
      STRATAVM_RETURN_IF_ERROR(r.read_varint_u32(&field_idx));
      STRATAVM_RETURN_IF_ERROR(r.read_varint_u32(&target_atom));
      rec.node_id = src_node_id;
      rec.field_idx = field_idx;
      rec.target_atom = target_atom;
      break;
    }

    case OpCode::DropNode: {
      std::uint32_t node_id = 0;
      STRATAVM_RETURN_IF_ERROR(r.read_varint_u32(&node_id));
      rec.node_id = node_id;
      break;
    }

    case OpCode::Snapshot: {
      std::uint32_t label_atom = 0;
      STRATAVM_RETURN_IF_ERROR(r.read_varint_u32(&label_atom));
      rec.name_atom = label_atom;
      break;
    }

    case OpCode::SetMeta: {
      std::uint32_t key_atom = 0, val_atom = 0;
      STRATAVM_RETURN_IF_ERROR(r.read_varint_u32(&key_atom));
      STRATAVM_RETURN_IF_ERROR(r.read_varint_u32(&val_atom));
      rec.meta_key = key_atom;
      rec.meta_val = val_atom;
      break;
    }

    case OpCode::Nop:
      // No operands.
      break;

    default:
      return fail(Code::BadOpcode, "unhandled opcode in decode_op");
  }

  return rec;
}

// ---------------------------------------------------------------------------
// encode_value
// ---------------------------------------------------------------------------

void JournalCompactor::encode_value(const Value& v, ByteWriter& w) {
  switch (v.type()) {
    case ValueType::Int:
      w.svarint(v.as_int());
      break;
    case ValueType::Float:
      w.f64(v.as_float());
      break;
    case ValueType::Atom:
      w.varint(static_cast<std::uint64_t>(v.as_atom()));
      break;
    case ValueType::Ref:
      w.varint(static_cast<std::uint64_t>(v.as_ref()));
      break;
    case ValueType::List: {
      const auto& lst = v.as_list();
      w.varint(static_cast<std::uint64_t>(lst.size()));
      for (NodeId nid : lst) {
        w.varint(static_cast<std::uint64_t>(nid));
      }
      break;
    }
    case ValueType::Invalid:
      // Should not happen for surviving ops; emit a zero int as fallback.
      w.svarint(0);
      break;
  }
}

// ---------------------------------------------------------------------------
// encode_op
// ---------------------------------------------------------------------------

void JournalCompactor::encode_op(const OpRecord& rec, ByteWriter& w) {
  w.u8(static_cast<std::uint8_t>(rec.op));

  switch (rec.op) {
    case OpCode::NewNode:
      w.varint(static_cast<std::uint64_t>(rec.kind));
      w.varint(static_cast<std::uint64_t>(rec.name_atom));
      break;

    case OpCode::SetField:
      w.varint(static_cast<std::uint64_t>(rec.node_id));
      w.varint(static_cast<std::uint64_t>(rec.field_idx));
      encode_value(rec.value, w);
      break;

    case OpCode::BeginGroup:
      w.varint(static_cast<std::uint64_t>(rec.name_atom));
      // Use actual surviving children count as the hint (already trimmed).
      w.varint(static_cast<std::uint64_t>(rec.group_children.size()));
      break;

    case OpCode::EndGroup:
      // No operands.
      break;

    case OpCode::AddChild:
      w.varint(static_cast<std::uint64_t>(rec.target_atom));
      break;

    case OpCode::LinkRef:
      w.varint(static_cast<std::uint64_t>(rec.node_id));
      w.varint(static_cast<std::uint64_t>(rec.field_idx));
      w.varint(static_cast<std::uint64_t>(rec.target_atom));
      break;

    case OpCode::DropNode:
      w.varint(static_cast<std::uint64_t>(rec.node_id));
      break;

    case OpCode::Snapshot:
      w.varint(static_cast<std::uint64_t>(rec.name_atom));
      break;

    case OpCode::SetMeta:
      w.varint(static_cast<std::uint64_t>(rec.meta_key));
      w.varint(static_cast<std::uint64_t>(rec.meta_val));
      break;

    case OpCode::Nop:
      // No operands.
      break;
  }
}

// ---------------------------------------------------------------------------
// compact
// ---------------------------------------------------------------------------

Result<CompactionResult> JournalCompactor::compact(ByteReader& r,
                                                    const Schema& schema,
                                                    const AtomPool& atoms) {
  (void)atoms;  // reserved for future use

  // -- Phase 1: read the op count -----------------------------------------
  std::uint32_t op_count = 0;
  STRATAVM_RETURN_IF_ERROR(r.read_varint_u32(&op_count));

  // -- Phase 2: decode all ops ---------------------------------------------
  // We track node_kinds inline during decode so SetField can look up types.
  std::vector<std::uint32_t> node_kinds;  // node_id → kind
  std::vector<OpRecord> group_stack;       // open group frames

  // Flat list of all decoded ops. BeginGroup/EndGroup pairs are stored as
  // separate records; the EndGroup record carries the collected children.
  // AddChild records are kept individually between Begin/End (for re-encoding)
  // but also reflected into the EndGroup record's group_children list so we
  // can easily count surviving children for the Begin hint.
  std::vector<OpRecord> ops;
  ops.reserve(op_count);

  for (std::uint32_t i = 0; i < op_count; ++i) {
    auto res = decode_op(r, schema, node_kinds, group_stack);
    if (res.is_error()) return res.status();
    ops.push_back(res.take());
  }

  // If group_stack is not empty the journal was malformed; treat as error.
  if (!group_stack.empty()) {
    return fail(Code::GroupUnderflow, "unclosed group at end of journal");
  }

  CompactionStats stats;
  stats.input_op_count = static_cast<std::uint32_t>(ops.size());

  // -- Phase 3: determine live node set ------------------------------------
  // A node is "live at output" if it was not dropped by DropNode. We do a
  // simple forward pass: NewNode adds to live, DropNode removes.
  std::unordered_set<std::uint32_t> live_nodes;
  {
    std::uint32_t next_id = 0;
    for (auto& rec : ops) {
      if (rec.op == OpCode::NewNode) {
        live_nodes.insert(next_id);
        ++next_id;
      } else if (rec.op == OpCode::DropNode) {
        live_nodes.erase(rec.node_id);
      }
    }
  }

  // -- Phase 4: mark dead ops ----------------------------------------------

  // 4a. Nops are always dead.
  for (auto& rec : ops) {
    if (rec.op == OpCode::Nop) {
      rec.dead = true;
      ++stats.nops_removed;
    }
  }

  // 4b. Find last-write for each (node_id, field_idx) pair.
  //     Map value: index of the LAST SetField for this (node, field).
  std::map<std::pair<std::uint32_t, std::uint32_t>, std::size_t> last_set_field;
  for (std::size_t i = 0; i < ops.size(); ++i) {
    const auto& rec = ops[i];
    if (rec.op == OpCode::SetField) {
      auto key = std::make_pair(rec.node_id, rec.field_idx);
      last_set_field[key] = i;
    }
  }

  // Mark earlier SetField ops on the same (node, field) as dead (redundant).
  // Track which (node, field) pairs we've already seen the last write for.
  std::map<std::pair<std::uint32_t, std::uint32_t>, bool> seen_last;
  // Iterate in reverse: the first SetField we see for a (node,field) in reverse
  // order IS the last one (last_set_field[key] == that index). Mark all others.
  for (std::size_t i = ops.size(); i-- > 0;) {
    auto& rec = ops[i];
    if (rec.op == OpCode::SetField) {
      auto key = std::make_pair(rec.node_id, rec.field_idx);
      if (seen_last.count(key)) {
        // We already passed the last write; this is a redundant earlier write.
        rec.dead = true;
        ++stats.redundant_field_sets_removed;
      } else {
        seen_last[key] = true;
        // This is the last write for this key. But if the node is dead, mark
        // it dead too (counted under dead_node_ops_removed below).
      }
    }
  }

  // 4c. Mark ops on dead nodes as dead.
  for (auto& rec : ops) {
    if (rec.dead) continue;
    bool node_op = (rec.op == OpCode::SetField ||
                    rec.op == OpCode::LinkRef);
    if (node_op && live_nodes.find(rec.node_id) == live_nodes.end()) {
      rec.dead = true;
      ++stats.dead_node_ops_removed;
    }
  }

  // 4d. Dead group ops: scan for BeginGroup/EndGroup pairs where all of the
  //     AddChild children reference dead nodes (i.e., the group owner itself
  //     is dead or all children are on dead nodes). In the wire format the
  //     group owner is identified by name atom; AddChild targets are also
  //     name atoms. Without a name→node liveness map we can conservatively
  //     remove a group pair when the EndGroup's surviving children list is
  //     empty.
  //
  // Strategy: pair up BeginGroup and EndGroup indices, and check if the
  // group has any surviving AddChild between them (after dead-node filtering).
  // If not, mark Begin + End + all intermediate AddChild dead.
  {
    // Build a stack-based pairing.
    struct GroupFrame {
      std::size_t begin_idx;
      std::size_t end_idx;
      std::vector<std::size_t> child_indices;
    };
    std::vector<GroupFrame> frame_stack;
    std::vector<GroupFrame> completed_frames;

    for (std::size_t i = 0; i < ops.size(); ++i) {
      if (ops[i].dead) continue;
      if (ops[i].op == OpCode::BeginGroup) {
        frame_stack.push_back({i, 0, {}});
      } else if (ops[i].op == OpCode::EndGroup) {
        if (!frame_stack.empty()) {
          frame_stack.back().end_idx = i;
          completed_frames.push_back(frame_stack.back());
          frame_stack.pop_back();
        }
      } else if (ops[i].op == OpCode::AddChild) {
        if (!frame_stack.empty()) {
          frame_stack.back().child_indices.push_back(i);
        }
      }
    }

    // For each completed frame, check if there are surviving AddChild ops.
    for (auto& frame : completed_frames) {
      bool has_live_child = false;
      for (std::size_t cidx : frame.child_indices) {
        if (!ops[cidx].dead) {
          has_live_child = true;
          break;
        }
      }
      if (!has_live_child) {
        // Mark the entire group dead.
        ops[frame.begin_idx].dead = true;
        ops[frame.end_idx].dead = true;
        ++stats.dead_group_ops_removed;
        ++stats.dead_group_ops_removed;  // once for Begin, once for End
        for (std::size_t cidx : frame.child_indices) {
          if (!ops[cidx].dead) {
            ops[cidx].dead = true;
            ++stats.dead_group_ops_removed;
          }
        }
      }
    }
  }

  // -- Phase 5: Re-encode surviving ops ------------------------------------
  // For BeginGroup records we need to update group_children to reflect
  // only surviving AddChild ops. We do a second pass to build per-group
  // surviving child lists indexed by their BeginGroup position.
  //
  // Actually, for re-encoding we emit Begin/AddChild.../End in order; the
  // BeginGroup hint is set to the count of surviving AddChild ops in the group.
  // We pre-compute that by walking pairs again.
  //
  // Build a map: begin_op_index → surviving child count.
  std::map<std::size_t, std::uint32_t> begin_surviving_children;
  {
    struct Frame2 {
      std::size_t begin_idx;
      std::uint32_t child_count;
    };
    std::vector<Frame2> stack2;
    for (std::size_t i = 0; i < ops.size(); ++i) {
      if (ops[i].dead) continue;
      if (ops[i].op == OpCode::BeginGroup) {
        stack2.push_back({i, 0});
      } else if (ops[i].op == OpCode::EndGroup) {
        if (!stack2.empty()) {
          begin_surviving_children[stack2.back().begin_idx] =
              stack2.back().child_count;
          stack2.pop_back();
        }
      } else if (ops[i].op == OpCode::AddChild) {
        if (!stack2.empty()) {
          ++stack2.back().child_count;
        }
      }
    }
  }

  ByteWriter body;
  std::uint32_t out_count = 0;

  for (std::size_t i = 0; i < ops.size(); ++i) {
    auto& rec = ops[i];
    if (rec.dead) continue;

    // For BeginGroup, patch the reserve_hint to reflect surviving children.
    if (rec.op == OpCode::BeginGroup) {
      auto it = begin_surviving_children.find(i);
      if (it != begin_surviving_children.end()) {
        rec.reserve_hint = it->second;
      }
    }

    encode_op(rec, body);
    ++out_count;
  }

  // Prepend op count varint.
  ByteWriter w;
  w.varint(static_cast<std::uint64_t>(out_count));
  w.append(body);

  stats.output_op_count = out_count;

  CompactionResult result;
  result.journal_bytes = w.take();
  result.stats = stats;
  return result;
}

}  // namespace stratavm
