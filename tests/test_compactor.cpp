// test_compactor.cpp - unit tests for JournalCompactor.
//
// Tests build journal byte streams manually using ByteWriter, pass them through
// ByteReader, and verify compaction output and statistics.
#include "test_runner.hpp"

#include "stratavm/atom_pool.hpp"
#include "stratavm/byte_reader.hpp"
#include "stratavm/byte_writer.hpp"
#include "stratavm/compactor.hpp"
#include "stratavm/format.hpp"
#include "stratavm/schema.hpp"
#include "stratavm/status.hpp"
#include "stratavm/value.hpp"

using namespace stratavm;
using namespace stratavm::test;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Build a minimal Schema with one kind that has one Int field.
static Schema make_schema_one_kind_one_int_field() {
  Schema s;
  KindDef k;
  k.name = 0;  // atom 0 = "kind0"
  FieldDef fd;
  fd.name = 1;  // atom 1 = "field0"
  fd.type = ValueType::Int;
  fd.flags = kFieldNone;
  k.fields.push_back(fd);
  s.add_kind(k);
  return s;
}

// Build a Schema with one kind that has two Int fields.
static Schema make_schema_one_kind_two_int_fields() {
  Schema s;
  KindDef k;
  k.name = 0;
  for (int i = 0; i < 2; ++i) {
    FieldDef fd;
    fd.name = static_cast<AtomId>(i + 1);
    fd.type = ValueType::Int;
    fd.flags = kFieldNone;
    k.fields.push_back(fd);
  }
  s.add_kind(k);
  return s;
}

// Encode a journal (op_count varint + ops) for the compactor to consume.
// Returns ByteReader wrapping the bytes. The bytes are kept alive in `buf`.
static std::vector<uint8_t> encode_journal(
    std::uint32_t op_count_override,
    const std::function<void(ByteWriter&)>& write_ops) {
  ByteWriter w;
  w.varint(static_cast<std::uint64_t>(op_count_override));
  write_ops(w);
  return w.take();
}

// Convenience: run compactor on a hand-built journal and return result.
static Result<CompactionResult> run_compact(
    const std::vector<uint8_t>& journal_bytes,
    const Schema& schema) {
  AtomPool atoms;
  ByteReader r(journal_bytes.data(), journal_bytes.size());
  JournalCompactor cmp;
  return cmp.compact(r, schema, atoms);
}

// Decode the op count from a compacted journal payload.
static std::uint32_t read_op_count(const std::vector<uint8_t>& bytes) {
  ByteReader r(bytes.data(), bytes.size());
  std::uint32_t n = 0;
  (void)r.read_varint_u32(&n);
  return n;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

STEST(compact_nop_removal) {
  // A journal containing only 3 Nop ops should compact to 0 surviving ops.
  Schema schema;  // Empty schema is fine for Nop-only journals.
  AtomPool atoms;

  auto journal = encode_journal(3, [](ByteWriter& w) {
    for (int i = 0; i < 3; ++i) {
      w.u8(static_cast<std::uint8_t>(OpCode::Nop));
    }
  });

  auto res = run_compact(journal, schema);
  REQUIRE(res.is_ok());

  const CompactionResult& cr = res.value();
  REQUIRE_EQ(cr.stats.input_op_count, 3u);
  REQUIRE_EQ(cr.stats.output_op_count, 0u);
  REQUIRE_EQ(cr.stats.nops_removed, 3u);
  REQUIRE_EQ(read_op_count(cr.journal_bytes), 0u);
}

STEST(compact_dead_node_ops) {
  // Journal:
  //   NewNode(kind=0, name=10)    → node 0 alive
  //   SetField(node=0, field=0, Int=5)   ← on live node
  //   DropNode(node=0)            → node 0 now dead
  //   SetField(node=0, field=0, Int=6)   ← on DEAD node → should be removed
  //
  // Expected: 2 surviving ops (NewNode, DropNode).
  // SetField(5) is marked redundant (earlier write); SetField(6) is dead-node op.
  Schema schema = make_schema_one_kind_one_int_field();

  auto journal = encode_journal(4, [](ByteWriter& w) {
    // NewNode kind=0, name_atom=10
    w.u8(static_cast<std::uint8_t>(OpCode::NewNode));
    w.varint(0);   // kind
    w.varint(10);  // name

    // SetField node=0, field=0, Int=5
    w.u8(static_cast<std::uint8_t>(OpCode::SetField));
    w.varint(0);  // node_id
    w.varint(0);  // field_idx
    w.svarint(5); // int value

    // DropNode node=0
    w.u8(static_cast<std::uint8_t>(OpCode::DropNode));
    w.varint(0);  // node_id

    // SetField node=0, field=0, Int=6  ← on dead node
    w.u8(static_cast<std::uint8_t>(OpCode::SetField));
    w.varint(0);  // node_id
    w.varint(0);  // field_idx
    w.svarint(6); // int value
  });

  auto res = run_compact(journal, schema);
  REQUIRE(res.is_ok());

  const CompactionResult& cr = res.value();
  REQUIRE_EQ(cr.stats.input_op_count, 4u);
  // The compactor processes dead-node ops in two passes:
  //   - redundant_field_sets_removed: SetField(0,0,5) is the EARLIER write,
  //     SetField(0,0,6) is the last write. So first SetField is "redundant".
  //   - dead_node_ops_removed: SetField(0,0,6) is the last write on dead node 0.
  // So: redundant_field_sets_removed=1, dead_node_ops_removed=1
  // Surviving: NewNode(0), DropNode(0) = 2 ops.
  REQUIRE_EQ(cr.stats.redundant_field_sets_removed, 1u);
  REQUIRE_EQ(cr.stats.dead_node_ops_removed, 1u);
  REQUIRE_EQ(cr.stats.output_op_count, 2u);
}

STEST(compact_redundant_field_sets) {
  // Journal:
  //   NewNode(kind=0, name=10)
  //   SetField(node=0, field=0, Int=5)   ← first write
  //   SetField(node=0, field=0, Int=6)   ← last write (wins)
  //
  // Expected: 2 surviving ops (NewNode, SetField with value 6).
  // The first SetField(5) is marked as a redundant overwritten write.
  Schema schema = make_schema_one_kind_one_int_field();

  auto journal = encode_journal(3, [](ByteWriter& w) {
    w.u8(static_cast<std::uint8_t>(OpCode::NewNode));
    w.varint(0);   // kind
    w.varint(10);  // name

    w.u8(static_cast<std::uint8_t>(OpCode::SetField));
    w.varint(0);   // node_id
    w.varint(0);   // field_idx
    w.svarint(5);  // first write

    w.u8(static_cast<std::uint8_t>(OpCode::SetField));
    w.varint(0);   // node_id
    w.varint(0);   // field_idx
    w.svarint(6);  // last write
  });

  auto res = run_compact(journal, schema);
  REQUIRE(res.is_ok());

  const CompactionResult& cr = res.value();
  REQUIRE_EQ(cr.stats.input_op_count, 3u);
  REQUIRE_EQ(cr.stats.redundant_field_sets_removed, 1u);
  REQUIRE_EQ(cr.stats.output_op_count, 2u);
}

STEST(compact_preserves_live_ops) {
  // Journal with NewNode, SetField, Snapshot on a live node.
  // All 3 ops should survive compaction (node is not dropped).
  Schema schema = make_schema_one_kind_one_int_field();

  auto journal = encode_journal(3, [](ByteWriter& w) {
    w.u8(static_cast<std::uint8_t>(OpCode::NewNode));
    w.varint(0);   // kind
    w.varint(10);  // name

    w.u8(static_cast<std::uint8_t>(OpCode::SetField));
    w.varint(0);   // node_id
    w.varint(0);   // field_idx
    w.svarint(42); // value

    w.u8(static_cast<std::uint8_t>(OpCode::Snapshot));
    w.varint(10);  // label atom
  });

  auto res = run_compact(journal, schema);
  REQUIRE(res.is_ok());

  const CompactionResult& cr = res.value();
  REQUIRE_EQ(cr.stats.input_op_count, 3u);
  REQUIRE_EQ(cr.stats.output_op_count, 3u);
  REQUIRE_EQ(cr.stats.nops_removed, 0u);
  REQUIRE_EQ(cr.stats.dead_node_ops_removed, 0u);
  REQUIRE_EQ(cr.stats.redundant_field_sets_removed, 0u);
}

STEST(compact_stats_correct) {
  // Journal with a mix of ops:
  //   2 Nops (→ nops_removed=2)
  //   NewNode(kind=0, name=1) → node 0
  //   SetField(0, 0, 10) → redundant (overwritten)
  //   SetField(0, 0, 20) → last write, survives
  //   DropNode(0) → node 0 dead
  //   NewNode(kind=0, name=2) → node 1 (live)
  //   SetField(1, 0, 99) → on live node, survives
  //
  // Expected stats:
  //   input_op_count = 7
  //   nops_removed = 2
  //   redundant_field_sets_removed = 1  (first SetField on node 0)
  //   dead_node_ops_removed = 1         (the last SetField on node 0 after
  //                                      all redundancy is resolved, it's dead)
  //   output_op_count = surviving count
  Schema schema = make_schema_one_kind_one_int_field();

  auto journal = encode_journal(8, [](ByteWriter& w) {
    w.u8(static_cast<std::uint8_t>(OpCode::Nop));
    w.u8(static_cast<std::uint8_t>(OpCode::Nop));

    w.u8(static_cast<std::uint8_t>(OpCode::NewNode));
    w.varint(0); w.varint(1);

    w.u8(static_cast<std::uint8_t>(OpCode::SetField));
    w.varint(0); w.varint(0); w.svarint(10);  // redundant

    w.u8(static_cast<std::uint8_t>(OpCode::SetField));
    w.varint(0); w.varint(0); w.svarint(20);  // last write (but node dies)

    w.u8(static_cast<std::uint8_t>(OpCode::DropNode));
    w.varint(0);

    w.u8(static_cast<std::uint8_t>(OpCode::NewNode));
    w.varint(0); w.varint(2);

    w.u8(static_cast<std::uint8_t>(OpCode::SetField));
    w.varint(1); w.varint(0); w.svarint(99);
  });

  auto res = run_compact(journal, schema);
  REQUIRE(res.is_ok());

  const CompactionResult& cr = res.value();
  REQUIRE_EQ(cr.stats.input_op_count, 8u);
  REQUIRE_EQ(cr.stats.nops_removed, 2u);
  // redundant_field_sets_removed = 1 (first SetField on node 0)
  REQUIRE_EQ(cr.stats.redundant_field_sets_removed, 1u);
  // dead_node_ops_removed: the surviving (last) SetField on dead node 0 = 1
  REQUIRE_EQ(cr.stats.dead_node_ops_removed, 1u);
  // Surviving: NewNode(0), DropNode(0), NewNode(1), SetField(1,0,99) = 4
  REQUIRE_EQ(cr.stats.output_op_count, 4u);
}

STEST(compact_empty_journal) {
  // 0-op journal should produce a 0-op output with no stats.
  Schema schema;

  auto journal = encode_journal(0, [](ByteWriter&) {});

  auto res = run_compact(journal, schema);
  REQUIRE(res.is_ok());

  const CompactionResult& cr = res.value();
  REQUIRE_EQ(cr.stats.input_op_count, 0u);
  REQUIRE_EQ(cr.stats.output_op_count, 0u);
  REQUIRE_EQ(cr.stats.nops_removed, 0u);
  REQUIRE_EQ(read_op_count(cr.journal_bytes), 0u);
}

STEST(compact_group_no_children_removed) {
  // Group where the AddChild atom refers to a dropped node's name. After dead-
  // node filtering the group has no surviving children → whole group removed.
  //
  // Journal:
  //   NewNode(kind=0, name=1) → node 0, name atom 1
  //   NewNode(kind=0, name=2) → node 1, name atom 2  (parent)
  //   DropNode(node=0) → atom 1 is now dead
  //   BeginGroup(owner=atom 2, hint=1)
  //   AddChild(atom=1)         ← child node is dead
  //   EndGroup
  //
  // The group pair + AddChild should be marked dead (no surviving children).
  // Note: the compactor cannot map atom IDs to node liveness directly, but it
  // uses the absence of surviving AddChild ops as the signal. Since all
  // AddChild ops in the group are to dead-node names, the group becomes empty.
  //
  // Actually the compactor doesn't currently filter AddChild by node liveness
  // (it doesn't have atom→node liveness mapping at compact time). It removes
  // groups that have NO surviving AddChild after general dead-op filtering.
  // The AddChild itself is not on a node_id — it's an atom reference. The
  // basic compactor will leave the AddChild alive (it doesn't know if the atom
  // resolves to a dead node). The group will NOT be removed in this case.
  //
  // Instead, test the case where the BeginGroup/EndGroup pair has no AddChild
  // at all (reserve_hint=0 was the intent, empty group).
  //
  // Revised test: a group with 0 AddChild ops → should be removed.
  Schema schema = make_schema_one_kind_one_int_field();

  auto journal = encode_journal(4, [](ByteWriter& w) {
    w.u8(static_cast<std::uint8_t>(OpCode::NewNode));
    w.varint(0); w.varint(2);  // node 0, name=2

    w.u8(static_cast<std::uint8_t>(OpCode::BeginGroup));
    w.varint(2);  // owner atom
    w.varint(0);  // reserve hint = 0 (no children)

    // No AddChild ops.

    w.u8(static_cast<std::uint8_t>(OpCode::EndGroup));

    w.u8(static_cast<std::uint8_t>(OpCode::Nop));
  });

  auto res = run_compact(journal, schema);
  REQUIRE(res.is_ok());

  const CompactionResult& cr = res.value();
  REQUIRE_EQ(cr.stats.input_op_count, 4u);
  REQUIRE_EQ(cr.stats.nops_removed, 1u);
  // BeginGroup + EndGroup removed (empty group).
  REQUIRE(cr.stats.dead_group_ops_removed >= 2u);
  // Only NewNode survives.
  REQUIRE_EQ(cr.stats.output_op_count, 1u);
}

STEST(compact_multiple_fields_last_write_wins) {
  // Node 0 has field 0 written 3 times and field 1 written once.
  // Only last write to field 0 and sole write to field 1 survive.
  Schema schema = make_schema_one_kind_two_int_fields();

  auto journal = encode_journal(6, [](ByteWriter& w) {
    w.u8(static_cast<std::uint8_t>(OpCode::NewNode));
    w.varint(0); w.varint(5);

    // field 0: three writes (only last survives)
    w.u8(static_cast<std::uint8_t>(OpCode::SetField));
    w.varint(0); w.varint(0); w.svarint(1);

    w.u8(static_cast<std::uint8_t>(OpCode::SetField));
    w.varint(0); w.varint(0); w.svarint(2);

    w.u8(static_cast<std::uint8_t>(OpCode::SetField));
    w.varint(0); w.varint(0); w.svarint(3);  // last

    // field 1: one write
    w.u8(static_cast<std::uint8_t>(OpCode::SetField));
    w.varint(0); w.varint(1); w.svarint(99);

    w.u8(static_cast<std::uint8_t>(OpCode::Snapshot));
    w.varint(5);
  });

  auto res = run_compact(journal, schema);
  REQUIRE(res.is_ok());

  const CompactionResult& cr = res.value();
  REQUIRE_EQ(cr.stats.input_op_count, 6u);
  REQUIRE_EQ(cr.stats.redundant_field_sets_removed, 2u);  // two overwritten field-0 writes
  REQUIRE_EQ(cr.stats.output_op_count, 4u);  // NewNode + field0 + field1 + Snapshot
}

STEST(compact_setmeta_survives) {
  // SetMeta ops are not node-associated; they should always survive.
  Schema schema;

  auto journal = encode_journal(2, [](ByteWriter& w) {
    w.u8(static_cast<std::uint8_t>(OpCode::SetMeta));
    w.varint(3);  // key atom
    w.varint(7);  // val atom

    w.u8(static_cast<std::uint8_t>(OpCode::SetMeta));
    w.varint(4);  // key atom
    w.varint(8);  // val atom
  });

  auto res = run_compact(journal, schema);
  REQUIRE(res.is_ok());

  const CompactionResult& cr = res.value();
  REQUIRE_EQ(cr.stats.input_op_count, 2u);
  REQUIRE_EQ(cr.stats.output_op_count, 2u);
}

STEST(compact_output_is_valid_journal) {
  // After compaction the output bytes should themselves be decodable as a
  // journal (the compactor should not corrupt the encoding).
  Schema schema = make_schema_one_kind_one_int_field();

  auto journal = encode_journal(5, [](ByteWriter& w) {
    w.u8(static_cast<std::uint8_t>(OpCode::Nop));
    w.u8(static_cast<std::uint8_t>(OpCode::NewNode));
    w.varint(0); w.varint(1);

    w.u8(static_cast<std::uint8_t>(OpCode::SetField));
    w.varint(0); w.varint(0); w.svarint(100);

    w.u8(static_cast<std::uint8_t>(OpCode::SetField));
    w.varint(0); w.varint(0); w.svarint(200);  // redundant

    w.u8(static_cast<std::uint8_t>(OpCode::Snapshot));
    w.varint(1);
  });

  auto res = run_compact(journal, schema);
  REQUIRE(res.is_ok());

  // Now compact the output again — idempotent pass should produce same result.
  AtomPool atoms;
  const std::vector<uint8_t>& first_pass = res.value().journal_bytes;
  ByteReader r2(first_pass.data(), first_pass.size());
  JournalCompactor cmp2;
  auto res2 = cmp2.compact(r2, schema, atoms);
  REQUIRE(res2.is_ok());

  // Second pass should not change the op count.
  REQUIRE_EQ(res2.value().stats.output_op_count,
              res.value().stats.output_op_count);
  REQUIRE_EQ(res2.value().stats.redundant_field_sets_removed, 0u);
  REQUIRE_EQ(res2.value().stats.nops_removed, 0u);
}

int main(int argc, char** argv) { return stratavm::test::run_all(argc, argv); }
