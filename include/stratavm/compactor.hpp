// compactor.hpp - journal compaction: reads a raw journal byte stream and
// produces a shorter, semantically equivalent one.
//
// The compactor performs three main optimizations:
//   1. Remove Nop opcodes (they carry no semantic content).
//   2. Remove ops on dead nodes (nodes that were dropped before output).
//   3. Remove redundant SetField ops (last write to (node, field) wins).
//   4. Remove empty group constructs (BeginGroup/EndGroup pairs with no
//      surviving children after filtering).
#ifndef STRATAVM_COMPACTOR_HPP
#define STRATAVM_COMPACTOR_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "stratavm/atom_pool.hpp"
#include "stratavm/byte_reader.hpp"
#include "stratavm/byte_writer.hpp"
#include "stratavm/format.hpp"
#include "stratavm/schema.hpp"
#include "stratavm/status.hpp"
#include "stratavm/value.hpp"

namespace stratavm {

// One decoded journal operation. Fields are set only for the opcodes that
// logically carry them; unused fields remain zero/empty.
struct OpRecord {
  OpCode op = OpCode::Nop;

  // node_id: used by NewNode, SetField, LinkRef, DropNode, Snapshot (label is
  //          name_atom below for Snapshot; node_id holds the node for the rest)
  std::uint32_t node_id = 0;

  // kind: the KindId for NewNode
  std::uint32_t kind = 0;

  // name_atom: the name atom for NewNode, BeginGroup owner, Snapshot label
  std::uint32_t name_atom = 0;

  // field_idx: field ordinal for SetField and LinkRef
  std::uint32_t field_idx = 0;

  // value: the typed value carried by SetField
  Value value;

  // target_atom: the atom resolved as the target for LinkRef and AddChild;
  //              also the meta-value atom for SetMeta
  std::uint32_t target_atom = 0;

  // reserve_hint: the hint count from BeginGroup
  std::uint32_t reserve_hint = 0;

  // group_children: the AddChild atom entries accumulated for a group frame.
  // Populated when a group pair is fully decoded (at EndGroup time).
  std::vector<std::uint32_t> group_children;

  // meta_key / meta_val: atoms for SetMeta
  std::uint32_t meta_key = 0;
  std::uint32_t meta_val = 0;

  // dead: set by the compaction pass to suppress this op in output
  bool dead = false;
};

// Statistics collected by a single compact() call.
struct CompactionStats {
  std::uint32_t input_op_count = 0;
  std::uint32_t output_op_count = 0;
  std::uint32_t dead_node_ops_removed = 0;       // ops on dropped nodes
  std::uint32_t redundant_field_sets_removed = 0; // overwritten SetField ops
  std::uint32_t nops_removed = 0;                 // Nop opcodes dropped
  std::uint32_t dead_group_ops_removed = 0;       // empty group pairs removed
};

// The compaction result: a freshly encoded journal and the statistics for it.
struct CompactionResult {
  // Compacted journal payload (starts with a varint op_count, followed by
  // the surviving ops encoded in the same wire format as the input).
  std::vector<std::uint8_t> journal_bytes;

  CompactionStats stats;
};

class JournalCompactor {
 public:
  // Decode the journal from `r` (which must start with a varint op_count),
  // compact it, and return the result.  The Schema is needed to resolve field
  // types for SetField values; the AtomPool is accepted for future extensibility
  // but is not currently used by the compactor itself.
  Result<CompactionResult> compact(ByteReader& r, const Schema& schema,
                                   const AtomPool& atoms);

 private:
  // Decode a single op from the stream. `node_kinds` tracks (node_id → kind)
  // so that SetField can determine the correct ValueType for decoding.
  Result<OpRecord> decode_op(ByteReader& r, const Schema& schema,
                             std::vector<std::uint32_t>& node_kinds,
                             std::vector<OpRecord>& group_stack);

  // Re-encode a single surviving OpRecord back to the wire format.
  void encode_op(const OpRecord& rec, ByteWriter& w);

  // Encode one Value to the writer using the schema-determined wire format.
  void encode_value(const Value& v, ByteWriter& w);

  // Resolve the ValueType for a SetField op using node_kinds + schema.
  ValueType field_type_for(std::uint32_t node_id, std::uint32_t field_idx,
                            const std::vector<std::uint32_t>& node_kinds,
                            const Schema& schema) const;
};

}  // namespace stratavm

#endif  // STRATAVM_COMPACTOR_HPP
