#include "test_runner.hpp"
#include "stratavm/builder.hpp"
#include "stratavm/loader.hpp"
#include "stratavm/samples.hpp"

using namespace stratavm;
using namespace stratavm::test;

static bool round_trips(const std::vector<uint8_t>& bytes) {
  LoadOptions opt;
  auto res = load(bytes.data(), bytes.size(), opt);
  return res.is_ok();
}

STEST(builder_minimal_roundtrip) {
  REQUIRE(round_trips(sample_container_minimal()));
}

STEST(builder_groups_roundtrip) {
  REQUIRE(round_trips(sample_container_groups()));
}

STEST(builder_rich_roundtrip) {
  REQUIRE(round_trips(sample_container_rich()));
}

STEST(builder_stable_serialization) {
  // Same builder program serialized twice must produce identical bytes.
  auto a = sample_container_rich();
  auto b = sample_container_rich();
  REQUIRE_EQ(a.size(), b.size());
  REQUIRE(a == b);
}

STEST(builder_atom_dedup) {
  ContainerBuilder b;
  AtomId id1 = b.atom("hello");
  AtomId id2 = b.atom("world");
  AtomId id3 = b.atom("hello");
  REQUIRE_EQ(id1, id3);
  REQUIRE(id1 != id2);
}

STEST(builder_empty_journal) {
  ContainerBuilder b;
  AtomId a = b.atom("x");
  FieldDef fd; fd.name = a; fd.type = ValueType::Int; fd.flags = 0;
  b.define_kind(a, {fd});
  // No heap nodes, no journal ops.
  auto bytes = b.serialize();
  REQUIRE(round_trips(bytes));
}

STEST(builder_heap_only_no_journal_ops) {
  ContainerBuilder b;
  AtomId a_kind = b.atom("doc");
  AtomId a_val = b.atom("v");
  AtomId a_title = b.atom("title");
  FieldDef fd; fd.name = a_title; fd.type = ValueType::Int; fd.flags = 0;
  KindId k = b.define_kind(a_kind, {fd});
  NodeId n = b.heap_node(k, a_val);
  b.heap_int(n, 0, 999);
  auto bytes = b.serialize();
  auto res = load(bytes.data(), bytes.size(), LoadOptions{});
  REQUIRE(res.is_ok());
  const Value* v = res.value()->vm.nodes()[0].field(0);
  REQUIRE(v != nullptr);
  REQUIRE_EQ(v->as_int(), 999);
}

STEST(builder_op_count) {
  ContainerBuilder b;
  b.atom("x"); b.atom("y");
  b.define_kind(0, {});
  b.heap_node(0, 0);
  b.op_nop();
  b.op_nop();
  b.op_snapshot(1);
  REQUIRE_EQ(b.journal_op_count(), 3u);
}

STEST(builder_xref_and_meta) {
  ContainerBuilder b;
  AtomId a_k = b.atom("k"); AtomId a_n = b.atom("n"); AtomId a_m = b.atom("m");
  b.define_kind(a_k, {});
  NodeId nd = b.heap_node(0, a_n);
  b.xref_entry(a_n, nd);
  b.meta_entry(a_k, a_m);
  auto bytes = b.serialize();
  LoadOptions opt;
  auto res = load(bytes.data(), bytes.size(), opt);
  REQUIRE(res.is_ok());
  REQUIRE(res.value()->sections.find(SectionTag::Xref) != nullptr);
  REQUIRE(res.value()->sections.find(SectionTag::Meta) != nullptr);
}

int main(int argc, char** argv) { return run_all(argc, argv); }
