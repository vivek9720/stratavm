// test_diff.cpp - unit tests for the diff engine.
#include "test_runner.hpp"

#include "stratavm/atom_pool.hpp"
#include "stratavm/byte_reader.hpp"
#include "stratavm/byte_writer.hpp"
#include "stratavm/diff.hpp"
#include "stratavm/format.hpp"
#include "stratavm/node.hpp"
#include "stratavm/schema.hpp"
#include "stratavm/status.hpp"
#include "stratavm/value.hpp"

#include <vector>
#include <algorithm>

using namespace stratavm;
using namespace stratavm::test;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Make a live, named node with a given id/kind/name-atom.
static Node make_node(NodeId id, KindId kind, AtomId name) {
  Node n;
  n.id = id;
  n.kind = kind;
  n.name = name;
  n.alive = true;
  n.placeholder = false;
  n.generation = 0;
  return n;
}

// Build a simple Schema with one kind, one Int field.
static Schema make_schema() {
  Schema s;
  KindDef k;
  k.name = 0;
  FieldDef fd;
  fd.name = 1;
  fd.type = ValueType::Int;
  fd.flags = kFieldNone;
  k.fields.push_back(fd);
  s.add_kind(k);
  return s;
}

// ---------------------------------------------------------------------------
// Tests: diff_node_graphs
// ---------------------------------------------------------------------------

STEST(diff_empty_graphs) {
  Schema schema = make_schema();
  std::vector<Node> before;
  std::vector<Node> after;

  GraphDiff diff = diff_node_graphs(before, after, schema);
  REQUIRE(diff.empty());
  REQUIRE_EQ(diff.nodes_added, 0u);
  REQUIRE_EQ(diff.nodes_removed, 0u);
  REQUIRE_EQ(diff.nodes_modified, 0u);
  REQUIRE_EQ(diff.total_field_changes, 0u);
}

STEST(diff_added_node) {
  // before: no nodes; after: 1 node → 1 Added change.
  Schema schema = make_schema();
  std::vector<Node> before;
  std::vector<Node> after;

  Node n = make_node(0, 0, 10);  // atom 10 = name
  n.fields[0] = Value::make_int(42);
  after.push_back(n);

  GraphDiff diff = diff_node_graphs(before, after, schema);

  REQUIRE_EQ(diff.nodes_added, 1u);
  REQUIRE_EQ(diff.nodes_removed, 0u);
  REQUIRE_EQ(diff.nodes_modified, 0u);

  // Find the Added change.
  const NodeChange* added = nullptr;
  for (const auto& nc : diff.changes) {
    if (nc.change == NodeChangeKind::Added) {
      added = &nc;
      break;
    }
  }
  REQUIRE(added != nullptr);
  REQUIRE_EQ(added->name, 10u);
  REQUIRE_EQ(added->after_id, 0u);
  REQUIRE_EQ(added->before_id, kInvalidNode);
  REQUIRE_EQ(added->kind, 0u);
  REQUIRE_EQ(added->field_changes.size(), 1u);
  REQUIRE_EQ(added->field_changes[0].field_idx, 0u);
  REQUIRE_EQ(added->field_changes[0].before.type(), ValueType::Invalid);
  REQUIRE_EQ(added->field_changes[0].after.as_int(), 42);
}

STEST(diff_removed_node) {
  // before: 1 node; after: no nodes → 1 Removed change.
  Schema schema = make_schema();
  std::vector<Node> before;
  std::vector<Node> after;

  Node n = make_node(0, 0, 10);
  n.fields[0] = Value::make_int(7);
  before.push_back(n);

  GraphDiff diff = diff_node_graphs(before, after, schema);

  REQUIRE_EQ(diff.nodes_removed, 1u);
  REQUIRE_EQ(diff.nodes_added, 0u);
  REQUIRE_EQ(diff.nodes_modified, 0u);

  const NodeChange* removed = nullptr;
  for (const auto& nc : diff.changes) {
    if (nc.change == NodeChangeKind::Removed) {
      removed = &nc;
      break;
    }
  }
  REQUIRE(removed != nullptr);
  REQUIRE_EQ(removed->name, 10u);
  REQUIRE_EQ(removed->before_id, 0u);
  REQUIRE_EQ(removed->after_id, kInvalidNode);
}

STEST(diff_modified_field) {
  // Same named node in both graphs, but a field value changed.
  Schema schema = make_schema();
  std::vector<Node> before;
  std::vector<Node> after;

  Node nb = make_node(0, 0, 10);
  nb.fields[0] = Value::make_int(1);
  before.push_back(nb);

  Node na = make_node(0, 0, 10);
  na.fields[0] = Value::make_int(2);  // changed
  after.push_back(na);

  GraphDiff diff = diff_node_graphs(before, after, schema);

  REQUIRE_EQ(diff.nodes_modified, 1u);
  REQUIRE_EQ(diff.nodes_added, 0u);
  REQUIRE_EQ(diff.nodes_removed, 0u);
  REQUIRE_EQ(diff.total_field_changes, 1u);

  const NodeChange* mod = nullptr;
  for (const auto& nc : diff.changes) {
    if (nc.change == NodeChangeKind::Modified) {
      mod = &nc;
      break;
    }
  }
  REQUIRE(mod != nullptr);
  REQUIRE_EQ(mod->field_changes.size(), 1u);
  REQUIRE_EQ(mod->field_changes[0].field_idx, 0u);
  REQUIRE_EQ(mod->field_changes[0].before.as_int(), 1);
  REQUIRE_EQ(mod->field_changes[0].after.as_int(), 2);
}

STEST(diff_unchanged) {
  // Identical graphs → no Modified, no Added, no Removed.
  Schema schema = make_schema();
  std::vector<Node> before;
  std::vector<Node> after;

  Node n = make_node(0, 0, 10);
  n.fields[0] = Value::make_int(5);
  before.push_back(n);
  after.push_back(n);  // same node

  GraphDiff diff = diff_node_graphs(before, after, schema);

  REQUIRE_EQ(diff.nodes_added, 0u);
  REQUIRE_EQ(diff.nodes_removed, 0u);
  REQUIRE_EQ(diff.nodes_modified, 0u);

  // There should be one Unchanged change.
  bool found_unchanged = false;
  for (const auto& nc : diff.changes) {
    if (nc.change == NodeChangeKind::Unchanged) {
      found_unchanged = true;
      break;
    }
  }
  REQUIRE(found_unchanged);
}

STEST(diff_children_added) {
  // Parent node gains a child → Modified with children_added.
  Schema schema = make_schema();

  // Before: parent (name=10) with no children; child node (name=20) exists.
  std::vector<Node> before;
  Node parent_b = make_node(0, 0, 10);
  before.push_back(parent_b);
  Node child_b = make_node(1, 0, 20);
  before.push_back(child_b);

  // After: parent (name=10) gains child (name=20) in its children list.
  std::vector<Node> after;
  Node parent_a = make_node(0, 0, 10);
  parent_a.children.push_back(1);  // child node at index 1
  after.push_back(parent_a);
  Node child_a = make_node(1, 0, 20);
  after.push_back(child_a);

  GraphDiff diff = diff_node_graphs(before, after, schema);

  // Parent should be Modified.
  REQUIRE_EQ(diff.nodes_modified, 1u);

  const NodeChange* mod = nullptr;
  for (const auto& nc : diff.changes) {
    if (nc.change == NodeChangeKind::Modified && nc.name == 10u) {
      mod = &nc;
      break;
    }
  }
  REQUIRE(mod != nullptr);
  REQUIRE_EQ(mod->children_added.size(), 1u);
  REQUIRE_EQ(mod->children_added[0], 20u);
  REQUIRE(mod->children_removed.empty());
}

STEST(diff_children_removed) {
  // Parent node loses a child → Modified with children_removed.
  Schema schema = make_schema();

  // Before: parent with one child.
  std::vector<Node> before;
  Node parent_b = make_node(0, 0, 10);
  parent_b.children.push_back(1);
  before.push_back(parent_b);
  Node child_b = make_node(1, 0, 20);
  before.push_back(child_b);

  // After: parent with no children; child still exists (but not linked).
  std::vector<Node> after;
  Node parent_a = make_node(0, 0, 10);
  after.push_back(parent_a);
  Node child_a = make_node(1, 0, 20);
  after.push_back(child_a);

  GraphDiff diff = diff_node_graphs(before, after, schema);

  REQUIRE_EQ(diff.nodes_modified, 1u);

  const NodeChange* mod = nullptr;
  for (const auto& nc : diff.changes) {
    if (nc.change == NodeChangeKind::Modified && nc.name == 10u) {
      mod = &nc;
      break;
    }
  }
  REQUIRE(mod != nullptr);
  REQUIRE(mod->children_added.empty());
  REQUIRE_EQ(mod->children_removed.size(), 1u);
  REQUIRE_EQ(mod->children_removed[0], 20u);
}

STEST(diff_serialize_roundtrip) {
  // diff → serialize_diff → deserialize_diff should produce identical diff.
  Schema schema = make_schema();
  std::vector<Node> before;
  std::vector<Node> after;

  // before: node name=10 with field 0 = 1
  Node nb = make_node(0, 0, 10);
  nb.fields[0] = Value::make_int(1);
  before.push_back(nb);

  // after: node name=10 with field 0 = 2 (modified), plus new node name=20
  Node na = make_node(0, 0, 10);
  na.fields[0] = Value::make_int(2);
  after.push_back(na);

  Node na2 = make_node(1, 0, 20);
  na2.fields[0] = Value::make_int(99);
  after.push_back(na2);

  GraphDiff diff = diff_node_graphs(before, after, schema);

  // Serialize.
  std::vector<uint8_t> bytes = serialize_diff(diff, schema);
  REQUIRE(!bytes.empty());

  // Deserialize.
  ByteReader r(bytes.data(), bytes.size());
  auto res = deserialize_diff(r, schema);
  REQUIRE(res.is_ok());

  const GraphDiff& diff2 = res.value();
  REQUIRE_EQ(diff2.nodes_added, diff.nodes_added);
  REQUIRE_EQ(diff2.nodes_removed, diff.nodes_removed);
  REQUIRE_EQ(diff2.nodes_modified, diff.nodes_modified);
  REQUIRE_EQ(diff2.changes.size(), diff.changes.size());

  // Verify that for each change in diff there is a matching one in diff2.
  for (const auto& nc1 : diff.changes) {
    bool found = false;
    for (const auto& nc2 : diff2.changes) {
      if (nc2.name == nc1.name && nc2.change == nc1.change) {
        found = true;
        REQUIRE_EQ(nc2.before_id, nc1.before_id);
        REQUIRE_EQ(nc2.after_id, nc1.after_id);
        REQUIRE_EQ(nc2.kind, nc1.kind);
        REQUIRE_EQ(nc2.field_changes.size(), nc1.field_changes.size());
        break;
      }
    }
    REQUIRE(found);
  }
}

STEST(diff_stats_correct) {
  // Verify nodes_added, nodes_removed, nodes_modified counts.
  Schema schema = make_schema();

  // before: nodes name=10, name=20, name=30
  std::vector<Node> before;
  Node n10b = make_node(0, 0, 10); n10b.fields[0] = Value::make_int(1);
  Node n20b = make_node(1, 0, 20); n20b.fields[0] = Value::make_int(2);
  Node n30b = make_node(2, 0, 30); n30b.fields[0] = Value::make_int(3);
  before.push_back(n10b);
  before.push_back(n20b);
  before.push_back(n30b);

  // after: name=10 (same), name=20 (field changed), name=40 (new), name=30 removed
  std::vector<Node> after;
  Node n10a = make_node(0, 0, 10); n10a.fields[0] = Value::make_int(1);  // unchanged
  Node n20a = make_node(1, 0, 20); n20a.fields[0] = Value::make_int(99); // modified
  Node n40a = make_node(2, 0, 40); n40a.fields[0] = Value::make_int(7);  // added
  after.push_back(n10a);
  after.push_back(n20a);
  after.push_back(n40a);

  GraphDiff diff = diff_node_graphs(before, after, schema);

  REQUIRE_EQ(diff.nodes_added, 1u);    // name=40
  REQUIRE_EQ(diff.nodes_removed, 1u);  // name=30
  REQUIRE_EQ(diff.nodes_modified, 1u); // name=20
  // name=10 is Unchanged (not counted in any of the above)
  REQUIRE_EQ(diff.total_field_changes, 1u);  // name=20's field change
}

STEST(diff_dead_nodes_excluded) {
  // Alive=false nodes should not appear in the diff at all.
  Schema schema = make_schema();

  std::vector<Node> before;
  std::vector<Node> after;

  // Dead node in before
  Node dead_b = make_node(0, 0, 10);
  dead_b.alive = false;
  before.push_back(dead_b);

  // Dead node in after
  Node dead_a = make_node(0, 0, 20);
  dead_a.alive = false;
  after.push_back(dead_a);

  GraphDiff diff = diff_node_graphs(before, after, schema);
  REQUIRE(diff.empty());
  REQUIRE_EQ(diff.nodes_added, 0u);
  REQUIRE_EQ(diff.nodes_removed, 0u);
}

STEST(diff_unnamed_nodes_excluded) {
  // Nodes with kInvalidAtom as name should not be compared.
  Schema schema = make_schema();

  std::vector<Node> before;
  std::vector<Node> after;

  Node anon_b = make_node(0, 0, kInvalidAtom);
  anon_b.fields[0] = Value::make_int(5);
  before.push_back(anon_b);

  Node anon_a = make_node(0, 0, kInvalidAtom);
  anon_a.fields[0] = Value::make_int(6);
  after.push_back(anon_a);

  GraphDiff diff = diff_node_graphs(before, after, schema);
  // Both are anonymous; no diff.
  REQUIRE(diff.empty());
}

STEST(diff_to_journal_added_node) {
  // diff_to_journal for a diff with 1 Added node should emit NewNode + SetField.
  Schema schema = make_schema();
  AtomPool atoms;

  std::vector<Node> before;
  std::vector<Node> after;

  Node n = make_node(0, 0, 10);
  n.fields[0] = Value::make_int(77);
  after.push_back(n);

  GraphDiff diff = diff_node_graphs(before, after, schema);
  REQUIRE_EQ(diff.nodes_added, 1u);

  auto res = diff_to_journal(diff, schema, atoms);
  REQUIRE(res.is_ok());

  const std::vector<uint8_t>& jbytes = res.value();
  REQUIRE(!jbytes.empty());

  // Decode the op count.
  ByteReader r(jbytes.data(), jbytes.size());
  uint32_t op_count = 0;
  REQUIRE_OK(r.read_varint_u32(&op_count));
  // At minimum: NewNode + SetField = 2 ops.
  REQUIRE(op_count >= 2u);

  // Read first op: should be NewNode.
  uint8_t first_op = 0;
  REQUIRE_OK(r.read_u8(&first_op));
  REQUIRE_EQ(first_op, static_cast<uint8_t>(OpCode::NewNode));
}

STEST(diff_to_journal_removed_node) {
  // diff_to_journal for a Removed node emits DropNode.
  Schema schema = make_schema();
  AtomPool atoms;

  std::vector<Node> before;
  std::vector<Node> after;

  Node n = make_node(5, 0, 10);  // before_id = 5
  n.fields[0] = Value::make_int(1);
  before.push_back(n);

  GraphDiff diff = diff_node_graphs(before, after, schema);
  REQUIRE_EQ(diff.nodes_removed, 1u);

  auto res = diff_to_journal(diff, schema, atoms);
  REQUIRE(res.is_ok());

  ByteReader r(res.value().data(), res.value().size());
  uint32_t op_count = 0;
  REQUIRE_OK(r.read_varint_u32(&op_count));
  REQUIRE_EQ(op_count, 1u);

  uint8_t opbyte = 0;
  REQUIRE_OK(r.read_u8(&opbyte));
  REQUIRE_EQ(opbyte, static_cast<uint8_t>(OpCode::DropNode));

  uint32_t node_id = 0;
  REQUIRE_OK(r.read_varint_u32(&node_id));
  REQUIRE_EQ(node_id, 5u);  // before_id
}

STEST(diff_serialize_empty) {
  // Serializing an empty diff and deserializing gives an empty diff.
  Schema schema = make_schema();
  GraphDiff diff;

  auto bytes = serialize_diff(diff, schema);
  REQUIRE(!bytes.empty());  // at least the count varint

  ByteReader r(bytes.data(), bytes.size());
  auto res = deserialize_diff(r, schema);
  REQUIRE(res.is_ok());
  REQUIRE(res.value().empty());
}

int main(int argc, char** argv) { return stratavm::test::run_all(argc, argv); }
