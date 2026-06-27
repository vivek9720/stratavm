// test_query.cpp - unit tests for the Stratavm query engine.
//
// Tests are grouped into two categories:
//   1. Parser tests: verify that well-formed and ill-formed query strings are
//      handled correctly.
//   2. Evaluator tests: build small in-memory node graphs and verify that
//      the engine returns the expected NodeIds.
//
// In-memory graph construction:
//   - An AtomPool is built by calling intern() directly.
//   - A Schema is built by calling add_kind() with hand-constructed KindDef
//     structs.
//   - A vector<Node> is populated by hand.
//
// No container files are loaded; the test has no I/O dependency.

#include "test_runner.hpp"

#include "stratavm/atom_pool.hpp"
#include "stratavm/format.hpp"
#include "stratavm/node.hpp"
#include "stratavm/query.hpp"
#include "stratavm/schema.hpp"
#include "stratavm/status.hpp"
#include "stratavm/value.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

using namespace stratavm;
using namespace stratavm::test;

// ===========================================================================
// Helpers
// ===========================================================================

// Build a minimal Node with the given id, kind, and name atom.
static Node make_node(NodeId id, KindId kind, AtomId name) {
  Node n;
  n.id   = id;
  n.kind = kind;
  n.name = name;
  n.alive = true;
  n.placeholder = false;
  n.generation  = 0;
  return n;
}

// ===========================================================================
// 1. Parser tests
// ===========================================================================

STEST(query_parse_kind) {
  AtomPool atoms;
  Schema schema;
  std::vector<Node> nodes;
  QueryEngine eng(atoms, schema, nodes);

  auto r = eng.parse("kind:document");
  REQUIRE(r.is_ok());
  REQUIRE(r.value() != nullptr);
  REQUIRE(r.value()->kind == QueryNodeKind::KindMatch);
  REQUIRE_EQ(r.value()->atom_name, std::string("document"));
}

STEST(query_parse_and) {
  AtomPool atoms;
  Schema schema;
  std::vector<Node> nodes;
  QueryEngine eng(atoms, schema, nodes);

  auto r = eng.parse("kind:document AND has_child:paragraph");
  REQUIRE(r.is_ok());
  REQUIRE(r.value() != nullptr);
  REQUIRE(r.value()->kind == QueryNodeKind::And);
  REQUIRE_EQ(r.value()->children.size(), std::size_t(2));
  REQUIRE(r.value()->children[0]->kind == QueryNodeKind::KindMatch);
  REQUIRE(r.value()->children[1]->kind == QueryNodeKind::HasChild);
}

STEST(query_parse_not) {
  AtomPool atoms;
  Schema schema;
  std::vector<Node> nodes;
  QueryEngine eng(atoms, schema, nodes);

  auto r = eng.parse("NOT kind:image");
  REQUIRE(r.is_ok());
  REQUIRE(r.value() != nullptr);
  REQUIRE(r.value()->kind == QueryNodeKind::Not);
  REQUIRE_EQ(r.value()->children.size(), std::size_t(1));
  REQUIRE(r.value()->children[0]->kind == QueryNodeKind::KindMatch);
}

STEST(query_parse_field_eq) {
  AtomPool atoms;
  Schema schema;
  std::vector<Node> nodes;
  QueryEngine eng(atoms, schema, nodes);

  auto r = eng.parse("field:title=\"hello\"");
  REQUIRE(r.is_ok());
  REQUIRE(r.value() != nullptr);
  REQUIRE(r.value()->kind == QueryNodeKind::FieldCmp);
  REQUIRE_EQ(r.value()->atom_name, std::string("title"));
  REQUIRE(r.value()->use_atom_val);
  REQUIRE_EQ(r.value()->atom_val, std::string("hello"));
  REQUIRE(r.value()->cmp_op == CmpOp::Eq);
}

STEST(query_parse_error) {
  AtomPool atoms;
  Schema schema;
  std::vector<Node> nodes;
  QueryEngine eng(atoms, schema, nodes);

  // "kind:" with nothing after the colon is a parse error.
  auto r = eng.parse("kind:");
  REQUIRE(r.is_error());
  REQUIRE(r.status().code() == Code::BadValue);
}

STEST(query_parse_or) {
  AtomPool atoms;
  Schema schema;
  std::vector<Node> nodes;
  QueryEngine eng(atoms, schema, nodes);

  auto r = eng.parse("kind:foo OR kind:bar");
  REQUIRE(r.is_ok());
  REQUIRE(r.value()->kind == QueryNodeKind::Or);
}

STEST(query_parse_nested_parens) {
  AtomPool atoms;
  Schema schema;
  std::vector<Node> nodes;
  QueryEngine eng(atoms, schema, nodes);

  auto r = eng.parse("(kind:a AND kind:b) OR kind:c");
  REQUIRE(r.is_ok());
  REQUIRE(r.value()->kind == QueryNodeKind::Or);
  REQUIRE(r.value()->children[0]->kind == QueryNodeKind::And);
}

STEST(query_parse_alive) {
  AtomPool atoms;
  Schema schema;
  std::vector<Node> nodes;
  QueryEngine eng(atoms, schema, nodes);

  auto r = eng.parse("alive");
  REQUIRE(r.is_ok());
  REQUIRE(r.value()->kind == QueryNodeKind::AnyAlive);
}

STEST(query_parse_field_int_gt) {
  AtomPool atoms;
  Schema schema;
  std::vector<Node> nodes;
  QueryEngine eng(atoms, schema, nodes);

  auto r = eng.parse("field:order>5");
  REQUIRE(r.is_ok());
  REQUIRE(r.value()->kind == QueryNodeKind::FieldCmp);
  REQUIRE(r.value()->cmp_op == CmpOp::Gt);
  REQUIRE_EQ(r.value()->int_val, std::int64_t(5));
  REQUIRE(!r.value()->use_atom_val);
}

// ===========================================================================
// 2. Evaluator tests
// ===========================================================================

// Helper: build the graph used by most eval tests.
//
// Schema:
//   kind 0: "document"  fields: [0:"order" Int, 1:"title" Atom]
//   kind 1: "section"   fields: []
//   kind 2: "image"     fields: []
//
// Nodes:
//   0: document, name="doc_a", order=3, title=atom("intro")
//   1: document, name="doc_b", order=8, title=atom("summary"), child=[2]
//   2: section,  name="section", child of node 1
//   3: image,    name="img"

struct TestGraph {
  AtomPool atoms;
  Schema schema;
  std::vector<Node> nodes;

  // Atom ids for kind names
  AtomId a_document = kInvalidAtom;
  AtomId a_section  = kInvalidAtom;
  AtomId a_image    = kInvalidAtom;

  // Atom ids for field names
  AtomId a_order  = kInvalidAtom;
  AtomId a_title  = kInvalidAtom;

  // Atom ids for field values
  AtomId a_intro   = kInvalidAtom;
  AtomId a_summary = kInvalidAtom;

  // Atom ids for node names
  AtomId a_doc_a   = kInvalidAtom;
  AtomId a_doc_b   = kInvalidAtom;
  AtomId a_sec     = kInvalidAtom;
  AtomId a_img     = kInvalidAtom;

  // Kind ids
  KindId k_document = kInvalidKind;
  KindId k_section  = kInvalidKind;
  KindId k_image    = kInvalidKind;

  void build() {
    // ---- Intern all strings ----
    a_document = atoms.intern("document");
    a_section  = atoms.intern("section");
    a_image    = atoms.intern("image");
    a_order    = atoms.intern("order");
    a_title    = atoms.intern("title");
    a_intro    = atoms.intern("intro");
    a_summary  = atoms.intern("summary");
    a_doc_a    = atoms.intern("doc_a");
    a_doc_b    = atoms.intern("doc_b");
    a_sec      = atoms.intern("section");   // same text as kind name
    a_img      = atoms.intern("img");

    // ---- Build schema ----
    {
      KindDef kd;
      kd.name = a_document;
      // field 0: order (Int)
      FieldDef fd0;
      fd0.name  = a_order;
      fd0.type  = ValueType::Int;
      fd0.flags = kFieldNone;
      kd.fields.push_back(fd0);
      // field 1: title (Atom)
      FieldDef fd1;
      fd1.name  = a_title;
      fd1.type  = ValueType::Atom;
      fd1.flags = kFieldNone;
      kd.fields.push_back(fd1);
      k_document = schema.add_kind(kd);
    }
    {
      KindDef kd;
      kd.name = a_section;
      k_section = schema.add_kind(kd);
    }
    {
      KindDef kd;
      kd.name = a_image;
      k_image = schema.add_kind(kd);
    }

    // ---- Build nodes ----
    // Node 0: document "doc_a", order=3, title="intro"
    {
      Node n = make_node(0, k_document, a_doc_a);
      n.fields[0] = Value::make_int(3);
      n.fields[1] = Value::make_atom(a_intro);
      nodes.push_back(std::move(n));
    }
    // Node 1: document "doc_b", order=8, title="summary", child=[2]
    {
      Node n = make_node(1, k_document, a_doc_b);
      n.fields[0] = Value::make_int(8);
      n.fields[1] = Value::make_atom(a_summary);
      n.children.push_back(2);
      nodes.push_back(std::move(n));
    }
    // Node 2: section "section"
    {
      Node n = make_node(2, k_section, a_sec);
      nodes.push_back(std::move(n));
    }
    // Node 3: image "img"
    {
      Node n = make_node(3, k_image, a_img);
      nodes.push_back(std::move(n));
    }
  }
};

STEST(query_exec_kind_match) {
  TestGraph g;
  g.build();
  QueryEngine eng(g.atoms, g.schema, g.nodes);

  QueryResult res = eng.execute("kind:document");
  REQUIRE_EQ(res.matched, 2u);
  REQUIRE_EQ(res.nodes.size(), std::size_t(2));

  // Both document nodes should be present.
  bool has0 = std::find(res.nodes.begin(), res.nodes.end(), NodeId(0)) != res.nodes.end();
  bool has1 = std::find(res.nodes.begin(), res.nodes.end(), NodeId(1)) != res.nodes.end();
  REQUIRE(has0);
  REQUIRE(has1);
}

STEST(query_exec_has_child) {
  TestGraph g;
  g.build();
  QueryEngine eng(g.atoms, g.schema, g.nodes);

  // Node 1 has a child named "section"; node 0, 2, 3 do not.
  QueryResult res = eng.execute("has_child:section");
  REQUIRE_EQ(res.matched, 1u);
  REQUIRE_EQ(res.nodes[0], NodeId(1));
}

STEST(query_exec_and) {
  TestGraph g;
  g.build();
  QueryEngine eng(g.atoms, g.schema, g.nodes);

  // Only node 1 is a document AND has a section child.
  QueryResult res = eng.execute("kind:document AND has_child:section");
  REQUIRE_EQ(res.matched, 1u);
  REQUIRE_EQ(res.nodes[0], NodeId(1));
}

STEST(query_exec_field_int_cmp) {
  TestGraph g;
  g.build();
  QueryEngine eng(g.atoms, g.schema, g.nodes);

  // field:order>5 — node 0 has order=3, node 1 has order=8. Expect node 1.
  QueryResult res = eng.execute("field:order>5");
  REQUIRE_EQ(res.matched, 1u);
  REQUIRE_EQ(res.nodes[0], NodeId(1));
}

STEST(query_exec_not) {
  TestGraph g;
  g.build();
  QueryEngine eng(g.atoms, g.schema, g.nodes);

  // NOT kind:image — all alive nodes except node 3.
  QueryResult res = eng.execute("NOT kind:image");
  REQUIRE_EQ(res.matched, 3u);
  bool has3 = std::find(res.nodes.begin(), res.nodes.end(), NodeId(3)) != res.nodes.end();
  REQUIRE(!has3);
}

STEST(query_exec_or) {
  TestGraph g;
  g.build();
  QueryEngine eng(g.atoms, g.schema, g.nodes);

  // kind:image OR kind:section — nodes 2 and 3.
  QueryResult res = eng.execute("kind:image OR kind:section");
  REQUIRE_EQ(res.matched, 2u);
  bool has2 = std::find(res.nodes.begin(), res.nodes.end(), NodeId(2)) != res.nodes.end();
  bool has3 = std::find(res.nodes.begin(), res.nodes.end(), NodeId(3)) != res.nodes.end();
  REQUIRE(has2);
  REQUIRE(has3);
}

STEST(query_exec_dead_node_skipped) {
  TestGraph g;
  g.build();
  // Mark node 0 as dead.
  g.nodes[0].alive = false;
  QueryEngine eng(g.atoms, g.schema, g.nodes);

  // Scanning for documents: only node 1 should appear (node 0 is dead).
  QueryResult res = eng.execute("kind:document");
  REQUIRE_EQ(res.matched, 1u);
  REQUIRE_EQ(res.nodes[0], NodeId(1));
  // scanned should still count the dead node in the slot scan.
  REQUIRE(res.scanned >= 4u);
}

STEST(query_exec_alive) {
  TestGraph g;
  g.build();
  QueryEngine eng(g.atoms, g.schema, g.nodes);

  // "alive" matches all alive nodes (all 4 in this graph).
  QueryResult res = eng.execute("alive");
  REQUIRE_EQ(res.matched, 4u);
}

STEST(query_exec_unknown_kind) {
  TestGraph g;
  g.build();
  QueryEngine eng(g.atoms, g.schema, g.nodes);

  // Querying for a kind not in the atom pool should return 0 matches.
  QueryResult res = eng.execute("kind:nonexistent");
  REQUIRE_EQ(res.matched, 0u);
}

STEST(query_exec_field_eq_string) {
  TestGraph g;
  g.build();
  QueryEngine eng(g.atoms, g.schema, g.nodes);

  // field:title="intro" — node 0 has title=intro.
  QueryResult res = eng.execute("field:title=\"intro\"");
  REQUIRE_EQ(res.matched, 1u);
  REQUIRE_EQ(res.nodes[0], NodeId(0));
}

// ===========================================================================
// main
// ===========================================================================

int main(int argc, char** argv) { return stratavm::test::run_all(argc, argv); }
