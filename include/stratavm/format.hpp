// format.hpp - on-disk constants shared by the encoder and decoder.
//
// A Stratavm container (.svm) records the construction of a structured object
// graph. The file is a fixed header followed by a directory of sections; the
// payload sections describe an interned string pool (ATOM), a type/schema table
// (SCMA), an initial object heap (HEAP), a journal of mutation opcodes (JRNL), a
// precomputed cross-reference index (XREF) and free-form metadata (META).
//
// Loading a container means: parse the sections, seed the replay machine with
// the initial heap, then replay the journal to reconstruct the final object
// graph. The journal is what makes the format stateful - the same node can be
// created, populated, linked, snapshotted and dropped across many operations.
#ifndef STRATAVM_FORMAT_HPP
#define STRATAVM_FORMAT_HPP

#include <cstdint>

namespace stratavm {

// 8-byte file magic. Spelled out so a hex dump is self-describing.
constexpr char kMagic[8] = {'S', 'T', 'R', 'A', 'T', 'A', 'V', 'M'};
constexpr std::size_t kMagicLen = 8;

constexpr std::uint8_t kVersionMajor = 1;
constexpr std::uint8_t kVersionMinor = 0;

// Header flag bits.
enum HeaderFlags : std::uint16_t {
  kFlagNone = 0,
  kFlagHasXref = 1u << 0,   // an XREF section is present and authoritative
  kFlagStrict = 1u << 1,    // producer asserts every invariant holds
};

// Section tags. Stored as a 4-byte ASCII run in the directory entry.
enum class SectionTag : std::uint32_t {
  Atom = 0x4D4F5441,  // 'ATOM' little-endian
  Schema = 0x414D4353,  // 'SCMA'
  Heap = 0x50414548,  // 'HEAP'
  Journal = 0x4C4E524A,  // 'JRNL'
  Xref = 0x46455258,  // 'XREF'
  Meta = 0x4154454D,  // 'META'
  Unknown = 0,
};

// Encode a 4-char tag literal into the little-endian u32 we store on disk.
constexpr std::uint32_t make_tag(char a, char b, char c, char d) {
  return static_cast<std::uint32_t>(static_cast<std::uint8_t>(a)) |
         (static_cast<std::uint32_t>(static_cast<std::uint8_t>(b)) << 8) |
         (static_cast<std::uint32_t>(static_cast<std::uint8_t>(c)) << 16) |
         (static_cast<std::uint32_t>(static_cast<std::uint8_t>(d)) << 24);
}

// Field value types stored in the schema and used to drive value decoding.
enum class ValueType : std::uint8_t {
  Int = 0,    // zig-zag signed varint
  Float = 1,  // 8-byte IEEE-754
  Atom = 2,   // index into the atom pool
  Ref = 3,    // reference to another node (by id)
  List = 4,   // length-prefixed list of node references
  Invalid = 0xFF,
};

// Journal opcodes. The replay machine dispatches on these. Operand layout is
// documented next to each entry and implemented in journal_reader.cpp.
enum class OpCode : std::uint8_t {
  NewNode = 0x01,     // varint kind, varint name_atom
  SetField = 0x02,    // varint node_id, varint field_idx, value
  BeginGroup = 0x03,  // varint owner_name_atom, varint reserve_hint
  EndGroup = 0x04,    // (no operands) resolve queued child links
  AddChild = 0x05,    // varint child_name_atom (queued onto current group)
  LinkRef = 0x06,     // varint src_node_id, varint field_idx, varint target_atom
  DropNode = 0x07,    // varint node_id
  Snapshot = 0x08,    // varint label_atom
  SetMeta = 0x09,     // varint key_atom, varint value_atom
  Nop = 0x0A,         // (no operands)
};

// Decode limits. These bound memory and time so the fuzzer cannot trivially OOM
// or hang on a small input claiming an enormous count. They are generous enough
// that realistic containers are unaffected.
struct Limits {
  std::uint32_t max_sections = 64;
  std::uint32_t max_atoms = 1u << 20;
  std::uint32_t max_atom_len = 1u << 16;
  std::uint32_t max_kinds = 1u << 16;
  std::uint32_t max_fields_per_kind = 1u << 12;
  std::uint32_t max_nodes = 1u << 22;
  std::uint32_t max_journal_ops = 1u << 22;
  std::uint32_t max_list_len = 1u << 20;
  std::uint32_t max_group_depth = 256;
  std::uint32_t max_value_depth = 32;
};

}  // namespace stratavm

#endif  // STRATAVM_FORMAT_HPP
