#include "stratavm/builder.hpp"

#include "stratavm/section.hpp"

namespace stratavm {

ContainerBuilder::ContainerBuilder() = default;

AtomId ContainerBuilder::atom(const std::string& text) {
  auto it = atom_index_.find(text);
  if (it != atom_index_.end()) return it->second;
  AtomId id = static_cast<AtomId>(atoms_.size());
  atoms_.push_back(text);
  atom_index_.emplace(text, id);
  return id;
}

KindId ContainerBuilder::define_kind(AtomId name, std::vector<FieldDef> fields) {
  KindDef kd;
  kd.name = name;
  kd.fields = std::move(fields);
  KindId id = static_cast<KindId>(kinds_.size());
  kinds_.push_back(std::move(kd));
  return id;
}

NodeId ContainerBuilder::heap_node(KindId kind, AtomId name) {
  NodeId id = static_cast<NodeId>(heap_.size());
  HeapNodeSpec spec;
  spec.kind = kind;
  spec.name = name;
  heap_.push_back(std::move(spec));
  return id;
}

void ContainerBuilder::heap_int(NodeId node, FieldId field, std::int64_t v) {
  heap_[node].fields.push_back({field, enc_int(v)});
}
void ContainerBuilder::heap_float(NodeId node, FieldId field, double v) {
  heap_[node].fields.push_back({field, enc_float(v)});
}
void ContainerBuilder::heap_atom(NodeId node, FieldId field, AtomId v) {
  heap_[node].fields.push_back({field, enc_atom(v)});
}
void ContainerBuilder::heap_ref(NodeId node, FieldId field, NodeId v) {
  heap_[node].fields.push_back({field, enc_ref(v)});
}
void ContainerBuilder::heap_list(NodeId node, FieldId field,
                                 const std::vector<NodeId>& v) {
  heap_[node].fields.push_back({field, enc_list(v)});
}

// --- Journal ----------------------------------------------------------------

NodeId ContainerBuilder::op_new_node(KindId kind, AtomId name) {
  journal_body_.u8(static_cast<std::uint8_t>(OpCode::NewNode));
  journal_body_.varint(kind);
  journal_body_.varint(name);
  journal_ops_++;
  // The on-disk id of the new node is the running total of heap + previously
  // created journal nodes. We do not track that precisely here because callers
  // address nodes through ids they already hold; the count is advanced by tests
  // that need it via journal_op_count().
  return kInvalidNode;
}

void ContainerBuilder::emit_set_field(NodeId node, FieldId field,
                                      const std::vector<std::uint8_t>& value_bytes) {
  journal_body_.u8(static_cast<std::uint8_t>(OpCode::SetField));
  journal_body_.varint(node);
  journal_body_.varint(field);
  journal_body_.raw(value_bytes.data(), value_bytes.size());
  journal_ops_++;
}

void ContainerBuilder::op_set_int(NodeId node, FieldId field, std::int64_t v) {
  emit_set_field(node, field, enc_int(v));
}
void ContainerBuilder::op_set_float(NodeId node, FieldId field, double v) {
  emit_set_field(node, field, enc_float(v));
}
void ContainerBuilder::op_set_atom(NodeId node, FieldId field, AtomId v) {
  emit_set_field(node, field, enc_atom(v));
}
void ContainerBuilder::op_set_ref(NodeId node, FieldId field, NodeId v) {
  emit_set_field(node, field, enc_ref(v));
}
void ContainerBuilder::op_set_list(NodeId node, FieldId field,
                                   const std::vector<NodeId>& v) {
  emit_set_field(node, field, enc_list(v));
}

void ContainerBuilder::op_begin_group(AtomId owner_name, std::uint32_t reserve_hint) {
  journal_body_.u8(static_cast<std::uint8_t>(OpCode::BeginGroup));
  journal_body_.varint(owner_name);
  journal_body_.varint(reserve_hint);
  journal_ops_++;
}
void ContainerBuilder::op_add_child(AtomId child_name) {
  journal_body_.u8(static_cast<std::uint8_t>(OpCode::AddChild));
  journal_body_.varint(child_name);
  journal_ops_++;
}
void ContainerBuilder::op_end_group() {
  journal_body_.u8(static_cast<std::uint8_t>(OpCode::EndGroup));
  journal_ops_++;
}
void ContainerBuilder::op_link_ref(NodeId src, FieldId field, AtomId target_name) {
  journal_body_.u8(static_cast<std::uint8_t>(OpCode::LinkRef));
  journal_body_.varint(src);
  journal_body_.varint(field);
  journal_body_.varint(target_name);
  journal_ops_++;
}
void ContainerBuilder::op_drop_node(NodeId node) {
  journal_body_.u8(static_cast<std::uint8_t>(OpCode::DropNode));
  journal_body_.varint(node);
  journal_ops_++;
}
void ContainerBuilder::op_snapshot(AtomId label) {
  journal_body_.u8(static_cast<std::uint8_t>(OpCode::Snapshot));
  journal_body_.varint(label);
  journal_ops_++;
}
void ContainerBuilder::op_set_meta(AtomId key, AtomId value) {
  journal_body_.u8(static_cast<std::uint8_t>(OpCode::SetMeta));
  journal_body_.varint(key);
  journal_body_.varint(value);
  journal_ops_++;
}
void ContainerBuilder::op_nop() {
  journal_body_.u8(static_cast<std::uint8_t>(OpCode::Nop));
  journal_ops_++;
}

void ContainerBuilder::xref_entry(AtomId name, NodeId node) {
  xref_.emplace_back(name, node);
  emit_xref_ = true;
}
void ContainerBuilder::meta_entry(AtomId key, AtomId value) {
  meta_.emplace_back(key, value);
  emit_meta_ = true;
}

// --- Value encoders ---------------------------------------------------------

std::vector<std::uint8_t> ContainerBuilder::enc_int(std::int64_t v) {
  ByteWriter w;
  w.svarint(v);
  return w.take();
}
std::vector<std::uint8_t> ContainerBuilder::enc_float(double v) {
  ByteWriter w;
  w.f64(v);
  return w.take();
}
std::vector<std::uint8_t> ContainerBuilder::enc_atom(AtomId v) {
  ByteWriter w;
  w.varint(v);
  return w.take();
}
std::vector<std::uint8_t> ContainerBuilder::enc_ref(NodeId v) {
  ByteWriter w;
  w.varint(v);
  return w.take();
}
std::vector<std::uint8_t> ContainerBuilder::enc_list(const std::vector<NodeId>& v) {
  ByteWriter w;
  w.varint(v.size());
  for (NodeId id : v) w.varint(id);
  return w.take();
}

// --- Section payloads -------------------------------------------------------

ByteWriter ContainerBuilder::build_atom_section() const {
  ByteWriter w;
  w.varint(atoms_.size());
  for (const std::string& s : atoms_) w.lp_string(s);
  return w;
}

ByteWriter ContainerBuilder::build_schema_section() const {
  ByteWriter w;
  w.varint(kinds_.size());
  for (const KindDef& k : kinds_) {
    w.varint(k.name);
    w.varint(k.fields.size());
    for (const FieldDef& f : k.fields) {
      w.varint(f.name);
      w.u8(static_cast<std::uint8_t>(f.type));
      w.u8(f.flags);
    }
  }
  return w;
}

ByteWriter ContainerBuilder::build_heap_section() const {
  ByteWriter w;
  w.varint(heap_.size());
  for (std::size_t i = 0; i < heap_.size(); ++i) {
    const HeapNodeSpec& n = heap_[i];
    w.varint(i);
    w.varint(n.kind);
    w.varint(n.name);
    w.varint(n.fields.size());
    for (const FieldValue& fv : n.fields) {
      w.varint(fv.field);
      w.raw(fv.bytes.data(), fv.bytes.size());
    }
  }
  return w;
}

ByteWriter ContainerBuilder::build_journal_section() const {
  ByteWriter w;
  w.varint(journal_ops_);
  w.raw(journal_body_.bytes().data(), journal_body_.bytes().size());
  return w;
}

ByteWriter ContainerBuilder::build_xref_section() const {
  ByteWriter w;
  w.varint(xref_.size());
  for (const auto& e : xref_) {
    w.varint(e.first);
    w.varint(e.second);
  }
  return w;
}

ByteWriter ContainerBuilder::build_meta_section() const {
  ByteWriter w;
  w.varint(meta_.size());
  for (const auto& e : meta_) {
    w.varint(e.first);
    w.varint(e.second);
  }
  return w;
}

// --- Assembly ---------------------------------------------------------------

std::vector<std::uint8_t> ContainerBuilder::serialize() const {
  struct Built {
    std::uint32_t tag;
    ByteWriter payload;
  };
  std::vector<Built> sections;
  sections.push_back({static_cast<std::uint32_t>(SectionTag::Atom), build_atom_section()});
  sections.push_back({static_cast<std::uint32_t>(SectionTag::Schema), build_schema_section()});
  sections.push_back({static_cast<std::uint32_t>(SectionTag::Heap), build_heap_section()});
  sections.push_back({static_cast<std::uint32_t>(SectionTag::Journal), build_journal_section()});
  if (emit_xref_)
    sections.push_back({static_cast<std::uint32_t>(SectionTag::Xref), build_xref_section()});
  if (emit_meta_)
    sections.push_back({static_cast<std::uint32_t>(SectionTag::Meta), build_meta_section()});

  ByteWriter out;
  out.raw(std::string(kMagic, kMagicLen));
  out.u8(kVersionMajor);
  out.u8(kVersionMinor);
  out.u16(header_flags_);
  out.u32(static_cast<std::uint32_t>(sections.size()));

  // The directory comes immediately after the fixed header. We remember each
  // entry's offset/length slot positions so we can back-patch once the payloads
  // are placed.
  const std::size_t table_start = out.size();
  std::vector<std::size_t> offset_slots(sections.size());
  std::vector<std::size_t> length_slots(sections.size());
  for (std::size_t i = 0; i < sections.size(); ++i) {
    out.u32(sections[i].tag);
    out.u8(1);   // section version
    out.u8(0);   // reserved
    out.u16(0);  // flags
    offset_slots[i] = out.size();
    out.u32(0);  // offset placeholder
    length_slots[i] = out.size();
    out.u32(0);  // length placeholder
  }
  (void)table_start;

  // Append payloads and patch the directory.
  for (std::size_t i = 0; i < sections.size(); ++i) {
    std::uint32_t offset = static_cast<std::uint32_t>(out.size());
    const auto& bytes = sections[i].payload.bytes();
    out.raw(bytes.data(), bytes.size());
    out.patch_u32(offset_slots[i], offset);
    out.patch_u32(length_slots[i], static_cast<std::uint32_t>(bytes.size()));
  }
  return out.take();
}

}  // namespace stratavm
