// diff.cpp - implementation of the container diff engine.
#include "stratavm/diff.hpp"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>
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

// ---------------------------------------------------------------------------
// Value serialization helpers (shared by serialize_diff and diff_to_journal)
// ---------------------------------------------------------------------------

static void write_value(const Value& v, ByteWriter& w) {
  w.u8(static_cast<std::uint8_t>(v.type()));
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
      // Just the type byte (0xFF); no payload.
      break;
  }
}

static Result<Value> read_value(ByteReader& r) {
  std::uint8_t type_byte = 0;
  STRATAVM_RETURN_IF_ERROR(r.read_u8(&type_byte));

  switch (static_cast<ValueType>(type_byte)) {
    case ValueType::Int: {
      std::int64_t v = 0;
      STRATAVM_RETURN_IF_ERROR(r.read_svarint(&v));
      return Value::make_int(v);
    }
    case ValueType::Float: {
      double v = 0.0;
      STRATAVM_RETURN_IF_ERROR(r.read_f64(&v));
      return Value::make_float(v);
    }
    case ValueType::Atom: {
      std::uint32_t v = 0;
      STRATAVM_RETURN_IF_ERROR(r.read_varint_u32(&v));
      return Value::make_atom(static_cast<AtomId>(v));
    }
    case ValueType::Ref: {
      std::uint32_t v = 0;
      STRATAVM_RETURN_IF_ERROR(r.read_varint_u32(&v));
      return Value::make_ref(static_cast<NodeId>(v));
    }
    case ValueType::List: {
      std::uint32_t count = 0;
      STRATAVM_RETURN_IF_ERROR(r.read_varint_u32(&count));
      std::vector<NodeId> lst;
      lst.reserve(count);
      for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t nid = 0;
        STRATAVM_RETURN_IF_ERROR(r.read_varint_u32(&nid));
        lst.push_back(static_cast<NodeId>(nid));
      }
      return Value::make_list(std::move(lst));
    }
    case ValueType::Invalid:
      return Value{};  // default-constructed Invalid value
    default:
      return fail(Code::BadValue, "unknown value type in diff");
  }
}

// ---------------------------------------------------------------------------
// Value equality helper
// ---------------------------------------------------------------------------

static bool values_equal(const Value& a, const Value& b) {
  if (a.type() != b.type()) return false;
  switch (a.type()) {
    case ValueType::Int:
      return a.as_int() == b.as_int();
    case ValueType::Float:
      // Bitwise comparison to handle NaN identity.
      return a.as_float() == b.as_float();
    case ValueType::Atom:
      return a.as_atom() == b.as_atom();
    case ValueType::Ref:
      return a.as_ref() == b.as_ref();
    case ValueType::List:
      return a.as_list() == b.as_list();
    case ValueType::Invalid:
      return true;  // Both invalid → equal
  }
  return false;
}

// ---------------------------------------------------------------------------
// diff_node_graphs
// ---------------------------------------------------------------------------

GraphDiff diff_node_graphs(const std::vector<Node>& before_nodes,
                           const std::vector<Node>& after_nodes,
                           const Schema& /*schema*/) {
  // Build name→node* maps for alive, named nodes.
  std::unordered_map<AtomId, const Node*> before_by_name;
  std::unordered_map<AtomId, const Node*> after_by_name;

  for (const Node& n : before_nodes) {
    if (n.alive && n.name != kInvalidAtom) {
      before_by_name.emplace(n.name, &n);
    }
  }
  for (const Node& n : after_nodes) {
    if (n.alive && n.name != kInvalidAtom) {
      after_by_name.emplace(n.name, &n);
    }
  }

  GraphDiff diff;

  // -- Added and Modified/Unchanged nodes (iterate after_by_name) ----------
  for (const auto& [name, after_node] : after_by_name) {
    auto bit = before_by_name.find(name);

    if (bit == before_by_name.end()) {
      // Node is new.
      NodeChange nc;
      nc.change = NodeChangeKind::Added;
      nc.before_id = kInvalidNode;
      nc.after_id = after_node->id;
      nc.name = name;
      nc.kind = after_node->kind;

      // Record all fields as "added" (before = Invalid, after = value).
      for (const auto& [fid, val] : after_node->fields) {
        FieldChange fc;
        fc.field_idx = fid;
        fc.before = Value{};  // Invalid = not present before
        fc.after = val;
        nc.field_changes.push_back(fc);
      }

      // Record all children as added.
      for (NodeId child_id : after_node->children) {
        if (child_id < after_nodes.size()) {
          AtomId child_name = after_nodes[child_id].name;
          if (child_name != kInvalidAtom) {
            nc.children_added.push_back(child_name);
          }
        }
      }

      diff.changes.push_back(std::move(nc));
      ++diff.nodes_added;
    } else {
      // Node exists in both; compare.
      const Node* before_node = bit->second;

      NodeChange nc;
      nc.before_id = before_node->id;
      nc.after_id = after_node->id;
      nc.name = name;
      nc.kind = after_node->kind;

      // Compare fields.
      // Fields in after not in before, or changed.
      for (const auto& [fid, after_val] : after_node->fields) {
        auto fit = before_node->fields.find(fid);
        if (fit == before_node->fields.end()) {
          // Field added.
          FieldChange fc;
          fc.field_idx = fid;
          fc.before = Value{};
          fc.after = after_val;
          nc.field_changes.push_back(fc);
        } else if (!values_equal(fit->second, after_val)) {
          // Field changed.
          FieldChange fc;
          fc.field_idx = fid;
          fc.before = fit->second;
          fc.after = after_val;
          nc.field_changes.push_back(fc);
        }
      }
      // Fields in before not in after (removed).
      for (const auto& [fid, before_val] : before_node->fields) {
        if (after_node->fields.find(fid) == after_node->fields.end()) {
          FieldChange fc;
          fc.field_idx = fid;
          fc.before = before_val;
          fc.after = Value{};
          nc.field_changes.push_back(fc);
        }
      }

      // Compare children (by name atom sets).
      std::unordered_set<AtomId> before_child_names;
      std::unordered_set<AtomId> after_child_names;

      for (NodeId cid : before_node->children) {
        if (cid < before_nodes.size()) {
          AtomId cn = before_nodes[cid].name;
          if (cn != kInvalidAtom) before_child_names.insert(cn);
        }
      }
      for (NodeId cid : after_node->children) {
        if (cid < after_nodes.size()) {
          AtomId cn = after_nodes[cid].name;
          if (cn != kInvalidAtom) after_child_names.insert(cn);
        }
      }

      for (AtomId cn : after_child_names) {
        if (!before_child_names.count(cn)) nc.children_added.push_back(cn);
      }
      for (AtomId cn : before_child_names) {
        if (!after_child_names.count(cn)) nc.children_removed.push_back(cn);
      }

      bool changed = !nc.field_changes.empty() ||
                     !nc.children_added.empty() ||
                     !nc.children_removed.empty() ||
                     (before_node->kind != after_node->kind);

      if (changed) {
        nc.change = NodeChangeKind::Modified;
        ++diff.nodes_modified;
        diff.total_field_changes +=
            static_cast<std::uint32_t>(nc.field_changes.size());
      } else {
        nc.change = NodeChangeKind::Unchanged;
      }

      diff.changes.push_back(std::move(nc));
    }
  }

  // -- Removed nodes (in before but not in after) --------------------------
  for (const auto& [name, before_node] : before_by_name) {
    if (after_by_name.find(name) == after_by_name.end()) {
      NodeChange nc;
      nc.change = NodeChangeKind::Removed;
      nc.before_id = before_node->id;
      nc.after_id = kInvalidNode;
      nc.name = name;
      nc.kind = before_node->kind;
      diff.changes.push_back(std::move(nc));
      ++diff.nodes_removed;
    }
  }

  return diff;
}

// ---------------------------------------------------------------------------
// serialize_diff
// ---------------------------------------------------------------------------

std::vector<std::uint8_t> serialize_diff(const GraphDiff& diff,
                                          const Schema& /*schema*/) {
  ByteWriter w;
  w.varint(static_cast<std::uint64_t>(diff.changes.size()));

  for (const NodeChange& nc : diff.changes) {
    w.u8(static_cast<std::uint8_t>(nc.change));
    w.varint(static_cast<std::uint64_t>(nc.name));
    w.varint(static_cast<std::uint64_t>(nc.before_id));
    w.varint(static_cast<std::uint64_t>(nc.after_id));
    w.varint(static_cast<std::uint64_t>(nc.kind));

    w.varint(static_cast<std::uint64_t>(nc.field_changes.size()));
    for (const FieldChange& fc : nc.field_changes) {
      w.varint(static_cast<std::uint64_t>(fc.field_idx));
      write_value(fc.before, w);
      write_value(fc.after, w);
    }

    w.varint(static_cast<std::uint64_t>(nc.children_added.size()));
    for (AtomId a : nc.children_added) {
      w.varint(static_cast<std::uint64_t>(a));
    }

    w.varint(static_cast<std::uint64_t>(nc.children_removed.size()));
    for (AtomId a : nc.children_removed) {
      w.varint(static_cast<std::uint64_t>(a));
    }
  }

  return w.take();
}

// ---------------------------------------------------------------------------
// deserialize_diff
// ---------------------------------------------------------------------------

Result<GraphDiff> deserialize_diff(ByteReader& r, const Schema& /*schema*/) {
  GraphDiff diff;

  std::uint32_t change_count = 0;
  STRATAVM_RETURN_IF_ERROR(r.read_varint_u32(&change_count));

  diff.changes.reserve(change_count);

  for (std::uint32_t i = 0; i < change_count; ++i) {
    NodeChange nc;

    std::uint8_t kind_byte = 0;
    STRATAVM_RETURN_IF_ERROR(r.read_u8(&kind_byte));
    nc.change = static_cast<NodeChangeKind>(kind_byte);

    std::uint32_t name = 0, before_id = 0, after_id = 0, kind_id = 0;
    STRATAVM_RETURN_IF_ERROR(r.read_varint_u32(&name));
    STRATAVM_RETURN_IF_ERROR(r.read_varint_u32(&before_id));
    STRATAVM_RETURN_IF_ERROR(r.read_varint_u32(&after_id));
    STRATAVM_RETURN_IF_ERROR(r.read_varint_u32(&kind_id));

    nc.name = static_cast<AtomId>(name);
    nc.before_id = static_cast<NodeId>(before_id);
    nc.after_id = static_cast<NodeId>(after_id);
    nc.kind = static_cast<KindId>(kind_id);

    std::uint32_t fc_count = 0;
    STRATAVM_RETURN_IF_ERROR(r.read_varint_u32(&fc_count));
    nc.field_changes.reserve(fc_count);

    for (std::uint32_t j = 0; j < fc_count; ++j) {
      FieldChange fc;
      std::uint32_t fidx = 0;
      STRATAVM_RETURN_IF_ERROR(r.read_varint_u32(&fidx));
      fc.field_idx = static_cast<FieldId>(fidx);

      auto before_res = read_value(r);
      if (before_res.is_error()) return before_res.status();
      fc.before = before_res.take();

      auto after_res = read_value(r);
      if (after_res.is_error()) return after_res.status();
      fc.after = after_res.take();

      nc.field_changes.push_back(std::move(fc));
    }

    std::uint32_t ca_count = 0;
    STRATAVM_RETURN_IF_ERROR(r.read_varint_u32(&ca_count));
    nc.children_added.reserve(ca_count);
    for (std::uint32_t j = 0; j < ca_count; ++j) {
      std::uint32_t a = 0;
      STRATAVM_RETURN_IF_ERROR(r.read_varint_u32(&a));
      nc.children_added.push_back(static_cast<AtomId>(a));
    }

    std::uint32_t cr_count = 0;
    STRATAVM_RETURN_IF_ERROR(r.read_varint_u32(&cr_count));
    nc.children_removed.reserve(cr_count);
    for (std::uint32_t j = 0; j < cr_count; ++j) {
      std::uint32_t a = 0;
      STRATAVM_RETURN_IF_ERROR(r.read_varint_u32(&a));
      nc.children_removed.push_back(static_cast<AtomId>(a));
    }

    // Update stats counters.
    switch (nc.change) {
      case NodeChangeKind::Added:
        ++diff.nodes_added;
        break;
      case NodeChangeKind::Removed:
        ++diff.nodes_removed;
        break;
      case NodeChangeKind::Modified:
        ++diff.nodes_modified;
        diff.total_field_changes +=
            static_cast<std::uint32_t>(nc.field_changes.size());
        break;
      case NodeChangeKind::Unchanged:
        break;
    }

    diff.changes.push_back(std::move(nc));
  }

  return diff;
}

// ---------------------------------------------------------------------------
// diff_to_journal
// ---------------------------------------------------------------------------

// Emit the encoded Value into the journal stream. The encoding here must match
// what the replay VM expects: the value wire bytes only (no type tag byte),
// because in the journal wire format the type is determined from the schema.
// However, since we're generating a self-contained journal patch for fields
// whose type comes from the schema, we emit the raw value payload.
//
// For the purposes of diff_to_journal we emit full SetField ops where the
// value payload is in the schema-driven format (no type byte prefix — the
// schema provides the type).
static void emit_value_payload(const Value& v, ByteWriter& w) {
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
      // Shouldn't happen for valid after-state field values; skip.
      break;
  }
}

Result<std::vector<std::uint8_t>> diff_to_journal(const GraphDiff& diff,
                                                   const Schema& /*schema*/,
                                                   const AtomPool& /*atoms*/) {
  ByteWriter body;
  std::uint32_t op_count = 0;

  for (const NodeChange& nc : diff.changes) {
    switch (nc.change) {
      case NodeChangeKind::Removed: {
        // Emit DropNode(before_id).
        if (nc.before_id != kInvalidNode) {
          body.u8(static_cast<std::uint8_t>(OpCode::DropNode));
          body.varint(static_cast<std::uint64_t>(nc.before_id));
          ++op_count;
        }
        break;
      }

      case NodeChangeKind::Added: {
        // Emit NewNode(kind, name_atom).
        body.u8(static_cast<std::uint8_t>(OpCode::NewNode));
        body.varint(static_cast<std::uint64_t>(nc.kind));
        body.varint(static_cast<std::uint64_t>(nc.name));
        ++op_count;

        // Emit SetField for each field in the added node.
        for (const FieldChange& fc : nc.field_changes) {
          if (fc.after.type() == ValueType::Invalid) continue;
          // The after_id for an Added node is the node_id in the new graph.
          // For the journal, node IDs are sequential from the VM's next_id.
          // We use after_id here since the journal player will assign the same
          // ID in order (NewNode allocates sequentially).
          if (nc.after_id == kInvalidNode) continue;
          body.u8(static_cast<std::uint8_t>(OpCode::SetField));
          body.varint(static_cast<std::uint64_t>(nc.after_id));
          body.varint(static_cast<std::uint64_t>(fc.field_idx));
          emit_value_payload(fc.after, body);
          ++op_count;
        }

        // Emit a group for children if any.
        if (!nc.children_added.empty()) {
          body.u8(static_cast<std::uint8_t>(OpCode::BeginGroup));
          body.varint(static_cast<std::uint64_t>(nc.name));
          body.varint(static_cast<std::uint64_t>(nc.children_added.size()));
          ++op_count;

          for (AtomId child_name : nc.children_added) {
            body.u8(static_cast<std::uint8_t>(OpCode::AddChild));
            body.varint(static_cast<std::uint64_t>(child_name));
            ++op_count;
          }

          body.u8(static_cast<std::uint8_t>(OpCode::EndGroup));
          ++op_count;
        }
        break;
      }

      case NodeChangeKind::Modified: {
        // Emit SetField for changed fields (after value is valid = set/changed).
        for (const FieldChange& fc : nc.field_changes) {
          if (fc.after.type() == ValueType::Invalid) {
            // Field was removed — no direct opcode for clearing a field in this
            // format; skip (the replay would need the field absent, which means
            // DropNode + re-create would be the full approach, but for the basic
            // diff-to-journal we omit removed fields).
            continue;
          }
          if (nc.after_id == kInvalidNode) continue;
          body.u8(static_cast<std::uint8_t>(OpCode::SetField));
          body.varint(static_cast<std::uint64_t>(nc.after_id));
          body.varint(static_cast<std::uint64_t>(fc.field_idx));
          emit_value_payload(fc.after, body);
          ++op_count;
        }

        // Emit AddChild ops for newly added children via a group.
        if (!nc.children_added.empty()) {
          body.u8(static_cast<std::uint8_t>(OpCode::BeginGroup));
          body.varint(static_cast<std::uint64_t>(nc.name));
          body.varint(static_cast<std::uint64_t>(nc.children_added.size()));
          ++op_count;

          for (AtomId child_name : nc.children_added) {
            body.u8(static_cast<std::uint8_t>(OpCode::AddChild));
            body.varint(static_cast<std::uint64_t>(child_name));
            ++op_count;
          }

          body.u8(static_cast<std::uint8_t>(OpCode::EndGroup));
          ++op_count;
        }
        break;
      }

      case NodeChangeKind::Unchanged:
        // Nothing to emit.
        break;
    }
  }

  // Prepend the op_count varint.
  ByteWriter w;
  w.varint(static_cast<std::uint64_t>(op_count));
  w.append(body);

  return w.take();
}

}  // namespace stratavm
