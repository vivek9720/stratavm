// builder.hpp - an in-memory container author.
//
// The builder is the high-level encoder. Callers declare atoms, kinds, an
// initial heap and a journal program, then serialize() lays the whole thing out
// into a valid .svm byte stream: it computes section payloads, assembles the
// directory and back-patches each section's absolute offset and length.
//
// This is the same machinery the tests, the CLI `build` command and the seed
// generator use, which keeps every produced container honest with respect to
// what the decoder expects.
#ifndef STRATAVM_BUILDER_HPP
#define STRATAVM_BUILDER_HPP

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "stratavm/atom_pool.hpp"
#include "stratavm/byte_writer.hpp"
#include "stratavm/format.hpp"
#include "stratavm/schema.hpp"
#include "stratavm/value.hpp"

namespace stratavm {

class ContainerBuilder {
 public:
  ContainerBuilder();

  // --- Atom pool -----------------------------------------------------------
  AtomId atom(const std::string& text);

  // --- Schema --------------------------------------------------------------
  // Declare a kind with the given name and fields. Returns its kind id.
  KindId define_kind(AtomId name, std::vector<FieldDef> fields);

  // --- Initial heap --------------------------------------------------------
  // Append a heap node. Field setters below target the most recently added
  // heap node unless an explicit node handle is used.
  NodeId heap_node(KindId kind, AtomId name);
  void heap_int(NodeId node, FieldId field, std::int64_t v);
  void heap_float(NodeId node, FieldId field, double v);
  void heap_atom(NodeId node, FieldId field, AtomId v);
  void heap_ref(NodeId node, FieldId field, NodeId v);
  void heap_list(NodeId node, FieldId field, const std::vector<NodeId>& v);

  // --- Journal -------------------------------------------------------------
  NodeId op_new_node(KindId kind, AtomId name);
  void op_set_int(NodeId node, FieldId field, std::int64_t v);
  void op_set_float(NodeId node, FieldId field, double v);
  void op_set_atom(NodeId node, FieldId field, AtomId v);
  void op_set_ref(NodeId node, FieldId field, NodeId v);
  void op_set_list(NodeId node, FieldId field, const std::vector<NodeId>& v);
  void op_begin_group(AtomId owner_name, std::uint32_t reserve_hint);
  void op_add_child(AtomId child_name);
  void op_end_group();
  void op_link_ref(NodeId src, FieldId field, AtomId target_name);
  void op_drop_node(NodeId node);
  void op_snapshot(AtomId label);
  void op_set_meta(AtomId key, AtomId value);
  void op_nop();

  // --- Optional sections ---------------------------------------------------
  void xref_entry(AtomId name, NodeId node);
  void meta_entry(AtomId key, AtomId value);

  // Controls whether the XREF/META sections are emitted at all.
  void set_emit_xref(bool on) { emit_xref_ = on; }
  void set_emit_meta(bool on) { emit_meta_ = on; }
  void set_header_flags(std::uint16_t flags) { header_flags_ = flags; }

  // Serialize everything into a finished container.
  std::vector<std::uint8_t> serialize() const;

  // Number of journal ops emitted so far (used by tests).
  std::uint32_t journal_op_count() const { return journal_ops_; }

 private:
  struct FieldValue {
    FieldId field;
    std::vector<std::uint8_t> bytes;  // pre-encoded value payload
  };
  struct HeapNodeSpec {
    KindId kind;
    AtomId name;
    std::vector<FieldValue> fields;
  };

  // Value encoders shared by heap and journal field setters.
  static std::vector<std::uint8_t> enc_int(std::int64_t v);
  static std::vector<std::uint8_t> enc_float(double v);
  static std::vector<std::uint8_t> enc_atom(AtomId v);
  static std::vector<std::uint8_t> enc_ref(NodeId v);
  static std::vector<std::uint8_t> enc_list(const std::vector<NodeId>& v);

  void emit_set_field(NodeId node, FieldId field,
                      const std::vector<std::uint8_t>& value_bytes);

  ByteWriter build_atom_section() const;
  ByteWriter build_schema_section() const;
  ByteWriter build_heap_section() const;
  ByteWriter build_journal_section() const;
  ByteWriter build_xref_section() const;
  ByteWriter build_meta_section() const;

  std::vector<std::string> atoms_;
  std::unordered_map<std::string, AtomId> atom_index_;
  std::vector<KindDef> kinds_;
  std::vector<HeapNodeSpec> heap_;
  ByteWriter journal_body_;
  std::uint32_t journal_ops_ = 0;
  std::vector<std::pair<AtomId, NodeId>> xref_;
  std::vector<std::pair<AtomId, AtomId>> meta_;
  bool emit_xref_ = false;
  bool emit_meta_ = false;
  std::uint16_t header_flags_ = kFlagNone;
};

}  // namespace stratavm

#endif  // STRATAVM_BUILDER_HPP
