// test_field_index.cpp - unit tests for FieldIndex.
#include "test_runner.hpp"

#include "stratavm/atom_pool.hpp"
#include "stratavm/byte_reader.hpp"
#include "stratavm/byte_writer.hpp"
#include "stratavm/field_index.hpp"
#include "stratavm/format.hpp"
#include "stratavm/node.hpp"
#include "stratavm/schema.hpp"
#include "stratavm/status.hpp"
#include "stratavm/value.hpp"

#include <algorithm>
#include <vector>

using namespace stratavm;
using namespace stratavm::test;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Build a minimal Schema with one kind that has two fields:
//   field 0: Int
//   field 1: Atom
static Schema make_simple_schema(AtomPool& atoms) {
  AtomId a_kind  = atoms.intern("thing");
  AtomId a_order = atoms.intern("order");
  AtomId a_title = atoms.intern("title");

  ByteWriter w;
  w.varint(1);         // 1 kind
  w.varint(a_kind);    // kind name
  w.varint(2);         // 2 fields
  w.varint(a_order);   // field 0 name
  w.u8(static_cast<uint8_t>(ValueType::Int));
  w.u8(0);             // flags
  w.varint(a_title);   // field 1 name
  w.u8(static_cast<uint8_t>(ValueType::Atom));
  w.u8(0);

  auto bytes = w.take();
  ByteReader r(bytes.data(), bytes.size());
  Schema schema;
  Limits lim;
  (void)schema.decode(r, atoms, lim);  // ignore errors in test setup
  return schema;
}

// Construct a simple alive node with the given id/kind/name.
static Node make_node(NodeId id, KindId kind, AtomId name = kInvalidAtom) {
  Node n;
  n.id          = id;
  n.kind        = kind;
  n.name        = name;
  n.alive       = true;
  n.placeholder = false;
  n.generation  = 0;
  return n;
}

// ---------------------------------------------------------------------------
// Test: build from empty node list → 0 entries
// ---------------------------------------------------------------------------

STEST(field_index_build_empty) {
  AtomPool atoms;
  Schema schema = make_simple_schema(atoms);
  std::vector<Node> nodes;

  FieldIndex idx;
  idx.build(nodes, schema, atoms);

  REQUIRE_EQ(idx.entry_count(), 0u);
  REQUIRE_EQ(idx.node_count(), 0u);
}

// ---------------------------------------------------------------------------
// Test: lookup_int returns nodes matching (kind, field, value)
// ---------------------------------------------------------------------------

STEST(field_index_lookup_int) {
  AtomPool atoms;
  Schema schema = make_simple_schema(atoms);

  std::vector<Node> nodes;

  // nodes 0 and 1: order=5
  Node n0 = make_node(0, 0);
  n0.set_field(0, Value::make_int(5));
  nodes.push_back(n0);

  Node n1 = make_node(1, 0);
  n1.set_field(0, Value::make_int(5));
  nodes.push_back(n1);

  // node 2: order=10
  Node n2 = make_node(2, 0);
  n2.set_field(0, Value::make_int(10));
  nodes.push_back(n2);

  FieldIndex idx;
  idx.build(nodes, schema, atoms);

  // Lookup int field 0 == 5 → should return nodes 0 and 1.
  auto hits = idx.lookup_int(0, 0, 5);
  REQUIRE_EQ(hits.size(), 2u);

  // Both node IDs 0 and 1 must be present (order not guaranteed by spec).
  bool has0 = (std::find(hits.begin(), hits.end(), NodeId{0}) != hits.end());
  bool has1 = (std::find(hits.begin(), hits.end(), NodeId{1}) != hits.end());
  REQUIRE(has0);
  REQUIRE(has1);

  // Lookup int field 0 == 10 → only node 2.
  auto hits2 = idx.lookup_int(0, 0, 10);
  REQUIRE_EQ(hits2.size(), 1u);
  REQUIRE_EQ(hits2[0], NodeId{2});

  // Lookup int field 0 == 99 → empty.
  auto hits3 = idx.lookup_int(0, 0, 99);
  REQUIRE(hits3.empty());
}

// ---------------------------------------------------------------------------
// Test: lookup_atom returns nodes matching (kind, field, atom_id)
// ---------------------------------------------------------------------------

STEST(field_index_lookup_atom) {
  AtomPool atoms;
  Schema schema = make_simple_schema(atoms);

  AtomId a_intro = atoms.intern("intro");
  AtomId a_body  = atoms.intern("body");

  std::vector<Node> nodes;

  // nodes 0 and 1: title="intro"
  Node n0 = make_node(0, 0);
  n0.set_field(1, Value::make_atom(a_intro));
  nodes.push_back(n0);

  Node n1 = make_node(1, 0);
  n1.set_field(1, Value::make_atom(a_intro));
  nodes.push_back(n1);

  // node 2: title="body"
  Node n2 = make_node(2, 0);
  n2.set_field(1, Value::make_atom(a_body));
  nodes.push_back(n2);

  FieldIndex idx;
  idx.build(nodes, schema, atoms);

  // Lookup atom field 1 == "intro" → nodes 0 and 1.
  auto hits = idx.lookup_atom(0, 1, a_intro);
  REQUIRE_EQ(hits.size(), 2u);

  bool has0 = (std::find(hits.begin(), hits.end(), NodeId{0}) != hits.end());
  bool has1 = (std::find(hits.begin(), hits.end(), NodeId{1}) != hits.end());
  REQUIRE(has0);
  REQUIRE(has1);

  // Lookup atom field 1 == "body" → node 2.
  auto hits2 = idx.lookup_atom(0, 1, a_body);
  REQUIRE_EQ(hits2.size(), 1u);
  REQUIRE_EQ(hits2[0], NodeId{2});
}

// ---------------------------------------------------------------------------
// Test: lookup_has_field returns all nodes with that field set
// ---------------------------------------------------------------------------

STEST(field_index_has_field) {
  AtomPool atoms;
  Schema schema = make_simple_schema(atoms);

  std::vector<Node> nodes;

  // node 0 has field 0 only
  Node n0 = make_node(0, 0);
  n0.set_field(0, Value::make_int(1));
  nodes.push_back(n0);

  // node 1 has fields 0 and 1
  Node n1 = make_node(1, 0);
  n1.set_field(0, Value::make_int(2));
  n1.set_field(1, Value::make_atom(atoms.intern("x")));
  nodes.push_back(n1);

  // node 2 has no fields
  nodes.push_back(make_node(2, 0));

  FieldIndex idx;
  idx.build(nodes, schema, atoms);

  // Field 0 is set on nodes 0 and 1.
  auto hf0 = idx.lookup_has_field(0, 0);
  REQUIRE_EQ(hf0.size(), 2u);

  // Field 1 is set only on node 1.
  auto hf1 = idx.lookup_has_field(0, 1);
  REQUIRE_EQ(hf1.size(), 1u);
  REQUIRE_EQ(hf1[0], NodeId{1});

  // Field 2 does not exist at all.
  auto hf2 = idx.lookup_has_field(0, 2);
  REQUIRE(hf2.empty());
}

// ---------------------------------------------------------------------------
// Test: lookup_by_name finds the node with the matching name atom
// ---------------------------------------------------------------------------

STEST(field_index_by_name) {
  AtomPool atoms;
  Schema schema = make_simple_schema(atoms);

  AtomId a_alpha = atoms.intern("alpha");
  AtomId a_beta  = atoms.intern("beta");

  std::vector<Node> nodes;
  nodes.push_back(make_node(0, 0, a_alpha));
  nodes.push_back(make_node(1, 0, a_beta));
  nodes.push_back(make_node(2, 0, kInvalidAtom));  // no name

  FieldIndex idx;
  idx.build(nodes, schema, atoms);

  auto hits_alpha = idx.lookup_by_name(a_alpha);
  REQUIRE_EQ(hits_alpha.size(), 1u);
  REQUIRE_EQ(hits_alpha[0], NodeId{0});

  auto hits_beta = idx.lookup_by_name(a_beta);
  REQUIRE_EQ(hits_beta.size(), 1u);
  REQUIRE_EQ(hits_beta[0], NodeId{1});

  // kInvalidAtom should not appear in the name index.
  auto hits_none = idx.lookup_by_name(kInvalidAtom);
  REQUIRE(hits_none.empty());

  // Non-existent atom.
  auto hits_missing = idx.lookup_by_name(999u);
  REQUIRE(hits_missing.empty());
}

// ---------------------------------------------------------------------------
// Test: dead (alive=false) nodes are not indexed
// ---------------------------------------------------------------------------

STEST(field_index_skip_dead) {
  AtomPool atoms;
  Schema schema = make_simple_schema(atoms);

  std::vector<Node> nodes;

  // Alive node with field.
  Node n0 = make_node(0, 0);
  n0.set_field(0, Value::make_int(7));
  nodes.push_back(n0);

  // Dead node with same field value — must not appear in index.
  Node n1 = make_node(1, 0);
  n1.alive = false;
  n1.set_field(0, Value::make_int(7));
  nodes.push_back(n1);

  FieldIndex idx;
  idx.build(nodes, schema, atoms);

  // Only node 0 should be counted.
  REQUIRE_EQ(idx.node_count(), 1u);

  // Only node 0 in the int lookup.
  auto hits = idx.lookup_int(0, 0, 7);
  REQUIRE_EQ(hits.size(), 1u);
  REQUIRE_EQ(hits[0], NodeId{0});
}

// ---------------------------------------------------------------------------
// Test: placeholder nodes are also skipped
// ---------------------------------------------------------------------------

STEST(field_index_skip_placeholder) {
  AtomPool atoms;
  Schema schema = make_simple_schema(atoms);

  std::vector<Node> nodes;

  // Placeholder node.
  Node n0 = make_node(0, 0);
  n0.placeholder = true;
  n0.set_field(0, Value::make_int(3));
  nodes.push_back(n0);

  // Normal alive node.
  Node n1 = make_node(1, 0);
  n1.set_field(0, Value::make_int(3));
  nodes.push_back(n1);

  FieldIndex idx;
  idx.build(nodes, schema, atoms);

  REQUIRE_EQ(idx.node_count(), 1u);
  auto hits = idx.lookup_int(0, 0, 3);
  REQUIRE_EQ(hits.size(), 1u);
  REQUIRE_EQ(hits[0], NodeId{1});
}

// ---------------------------------------------------------------------------
// Test: for_each_in_kind iterates the correct entries
// ---------------------------------------------------------------------------

STEST(field_index_for_each_in_kind) {
  AtomPool atoms;
  Schema schema = make_simple_schema(atoms);

  AtomId a_x = atoms.intern("x");

  std::vector<Node> nodes;

  // Kind 0: two nodes with different fields.
  Node n0 = make_node(0, 0);
  n0.set_field(0, Value::make_int(10));
  nodes.push_back(n0);

  Node n1 = make_node(1, 0);
  n1.set_field(1, Value::make_atom(a_x));
  nodes.push_back(n1);

  // Kind 1: one node — should not appear when querying kind 0.
  Node n2 = make_node(2, 1);
  n2.set_field(0, Value::make_int(99));
  nodes.push_back(n2);

  FieldIndex idx;
  idx.build(nodes, schema, atoms);

  size_t count_kind0 = 0;
  bool   saw_int10   = false;
  bool   saw_atom_x  = false;

  idx.for_each_in_kind(0, [&](NodeId nid, FieldId fid, const Value& v) {
    ++count_kind0;
    if (nid == 0 && fid == 0 && v.type() == ValueType::Int && v.as_int() == 10)
      saw_int10 = true;
    if (nid == 1 && fid == 1 && v.type() == ValueType::Atom && v.as_atom() == a_x)
      saw_atom_x = true;
  });

  REQUIRE_EQ(count_kind0, 2u);  // exactly 2 entries for kind 0
  REQUIRE(saw_int10);
  REQUIRE(saw_atom_x);

  // Kind 1 should have exactly 1 entry.
  size_t count_kind1 = 0;
  idx.for_each_in_kind(1, [&](NodeId, FieldId, const Value&) { ++count_kind1; });
  REQUIRE_EQ(count_kind1, 1u);

  // Kind 99 does not exist — callback is never called.
  size_t count_kind99 = 0;
  idx.for_each_in_kind(99, [&](NodeId, FieldId, const Value&) { ++count_kind99; });
  REQUIRE_EQ(count_kind99, 0u);
}

// ---------------------------------------------------------------------------
// Test: clear() resets all state
// ---------------------------------------------------------------------------

STEST(field_index_clear) {
  AtomPool atoms;
  Schema schema = make_simple_schema(atoms);

  std::vector<Node> nodes;
  Node n0 = make_node(0, 0);
  n0.set_field(0, Value::make_int(42));
  nodes.push_back(n0);

  FieldIndex idx;
  idx.build(nodes, schema, atoms);

  // Verify something was indexed.
  REQUIRE(idx.entry_count() > 0u);
  REQUIRE(idx.node_count()  > 0u);

  idx.clear();

  REQUIRE_EQ(idx.entry_count(), 0u);
  REQUIRE_EQ(idx.node_count(),  0u);
  REQUIRE(idx.lookup_int(0, 0, 42).empty());
  REQUIRE(idx.lookup_has_field(0, 0).empty());
}

// ---------------------------------------------------------------------------
// Test: multiple kinds indexed independently
// ---------------------------------------------------------------------------

STEST(field_index_multi_kind) {
  AtomPool atoms;

  // Build a schema with 2 kinds, each with 1 Int field.
  AtomId a_k0 = atoms.intern("kind0");
  AtomId a_k1 = atoms.intern("kind1");
  AtomId a_f0 = atoms.intern("field0");

  ByteWriter w;
  w.varint(2);  // 2 kinds

  w.varint(a_k0);
  w.varint(1);  // 1 field
  w.varint(a_f0);
  w.u8(static_cast<uint8_t>(ValueType::Int));
  w.u8(0);

  w.varint(a_k1);
  w.varint(1);  // 1 field
  w.varint(a_f0);
  w.u8(static_cast<uint8_t>(ValueType::Int));
  w.u8(0);

  auto bytes = w.take();
  ByteReader r(bytes.data(), bytes.size());
  Schema schema;
  Limits lim;
  (void)schema.decode(r, atoms, lim);

  std::vector<Node> nodes;

  // Kind 0, field 0 = 5.
  Node n0 = make_node(0, 0);
  n0.set_field(0, Value::make_int(5));
  nodes.push_back(n0);

  // Kind 1, field 0 = 5 (same value, different kind).
  Node n1 = make_node(1, 1);
  n1.set_field(0, Value::make_int(5));
  nodes.push_back(n1);

  FieldIndex idx;
  idx.build(nodes, schema, atoms);

  // Lookup by kind 0 only returns node 0.
  auto hits0 = idx.lookup_int(0, 0, 5);
  REQUIRE_EQ(hits0.size(), 1u);
  REQUIRE_EQ(hits0[0], NodeId{0});

  // Lookup by kind 1 only returns node 1.
  auto hits1 = idx.lookup_int(1, 0, 5);
  REQUIRE_EQ(hits1.size(), 1u);
  REQUIRE_EQ(hits1[0], NodeId{1});
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char** argv) { return stratavm::test::run_all(argc, argv); }
