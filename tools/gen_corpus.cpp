// gen_corpus.cpp - writes seed files to fuzz/corpus/ directories.
//
// Usage: gen_corpus <container_corpus_dir> <journal_corpus_dir>
//
// This binary is compiled and run as part of `make gen-corpus`. It must not
// touch the network and must produce deterministic output.
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "stratavm/builder.hpp"
#include "stratavm/byte_writer.hpp"
#include "stratavm/env.hpp"
#include "stratavm/format.hpp"
#include "stratavm/node.hpp"
#include "stratavm/samples.hpp"
#include "stratavm/schema.hpp"

using namespace stratavm;

static bool write_file(const std::string& path, const std::vector<uint8_t>& data) {
  std::ofstream f(path, std::ios::binary);
  if (!f) return false;
  if (!data.empty()) f.write(reinterpret_cast<const char*>(data.data()), data.size());
  return !!f;
}

static bool emit(const std::string& dir, const std::string& name,
                 const std::vector<uint8_t>& data) {
  std::string path = dir + "/" + name;
  if (!write_file(path, data)) {
    std::fprintf(stderr, "failed to write %s\n", path.c_str());
    return false;
  }
  std::printf("  wrote %s (%zu bytes)\n", name.c_str(), data.size());
  return true;
}

// Build a container seed that exercises the group/forward-ref path SAFELY
// (reserve_hint equals the actual number of children added).
static std::vector<uint8_t> container_forward_ref_safe() {
  ContainerBuilder b;
  // Atom ids 0-17 mirror the default env ordering so journal seeds are aligned.
  AtomId a_document  = b.atom("document");   // 0
  AtomId a_section   = b.atom("section");    // 1
  AtomId a_paragraph = b.atom("paragraph");  // 2
  AtomId a_link      = b.atom("link");       // 3
  AtomId a_image     = b.atom("image");      // 4
  AtomId a_title     = b.atom("title");      // 5
  AtomId a_body      = b.atom("body");       // 6
  AtomId a_order     = b.atom("order");      // 7
  AtomId a_parent    = b.atom("parent");     // 8
  AtomId a_href      = b.atom("href");       // 9
  AtomId a_target    = b.atom("target");     // 10
  AtomId a_width     = b.atom("width");      // 11
  AtomId a_height    = b.atom("height");     // 12
  AtomId a_root      = b.atom("root");       // 13
  AtomId a_main      = b.atom("main");       // 14
  AtomId a_intro     = b.atom("intro");      // 15
  AtomId a_appendix  = b.atom("appendix");   // 16
  AtomId a_figure    = b.atom("figure");     // 17
  (void)a_link; (void)a_image; (void)a_href; (void)a_target;
  (void)a_width; (void)a_height; (void)a_figure;

  auto fd = [](AtomId name, ValueType t, uint8_t flags) {
    FieldDef f; f.name = name; f.type = t; f.flags = flags; return f;
  };
  KindId k_doc = b.define_kind(a_document, {
    fd(a_title, ValueType::Atom, kFieldIndexed),
    fd(a_body,  ValueType::Ref,  kFieldNone),
    fd(a_order, ValueType::Int,  kFieldNone)
  });
  KindId k_sec = b.define_kind(a_section, {
    fd(a_title,  ValueType::Atom, kFieldIndexed),
    fd(a_parent, ValueType::Ref,  kFieldNone),
    fd(a_order,  ValueType::Int,  kFieldNone)
  });
  KindId k_par = b.define_kind(a_paragraph, {
    fd(a_body, ValueType::Atom, kFieldNone)
  });

  // Heap: root doc node 0.
  NodeId doc = b.heap_node(k_doc, a_root);
  b.heap_atom(doc, 0, a_main);
  b.heap_int(doc, 2, 1);

  // Journal: two paragraphs, THEN group them under root.
  // Both names (intro, appendix) exist before the group opens -> no placeholders.
  NodeId next = 1;
  b.op_new_node(k_par, a_intro);   NodeId p1 = next++;
  b.op_set_atom(p1, 0, a_body);
  b.op_new_node(k_par, a_appendix); NodeId p2 = next++;
  b.op_set_atom(p2, 0, a_body);

  b.op_begin_group(a_root, 4); // reserve_hint >= children: safe
  b.op_add_child(a_intro);
  b.op_add_child(a_appendix);
  b.op_end_group();

  // A section with a parent ref.
  b.op_new_node(k_sec, a_main); NodeId sec = next++;
  b.op_set_atom(sec, 0, a_title);
  b.op_set_ref(sec, 1, doc);
  b.op_set_int(sec, 2, 2);

  b.op_snapshot(a_root);
  b.op_snapshot(a_main);
  b.op_set_meta(a_title, a_main);
  (void)p2;
  return b.serialize();
}

// Container seed that exercises XREF and META sections.
static std::vector<uint8_t> container_with_xref() {
  auto bytes = sample_container_rich();
  return bytes;
}

// Journal seed: exercises BEGIN_GROUP with existing children (no placeholders).
static std::vector<uint8_t> journal_group_existing() {
  return sample_journal_groups();
}

// Journal seed: interleaved new nodes + set_field + snapshot.
static std::vector<uint8_t> journal_multiop() {
  ByteWriter w;
  const uint32_t N = 9;
  w.varint(N);
  // NEW_NODE kind=0(doc) name=0(document)
  w.u8(static_cast<uint8_t>(OpCode::NewNode)); w.varint(0); w.varint(0);
  // SET_FIELD node=0 field=0(title,Atom) = atom 1
  w.u8(static_cast<uint8_t>(OpCode::SetField)); w.varint(0); w.varint(0); w.varint(1);
  // SET_FIELD node=0 field=2(order,Int) = 42
  w.u8(static_cast<uint8_t>(OpCode::SetField)); w.varint(0); w.varint(2); w.svarint(42);
  // NEW_NODE kind=2(para) name=2(paragraph)
  w.u8(static_cast<uint8_t>(OpCode::NewNode)); w.varint(2); w.varint(2);
  // BEGIN_GROUP owner=0(document) hint=1
  w.u8(static_cast<uint8_t>(OpCode::BeginGroup)); w.varint(0); w.varint(1);
  // ADD_CHILD 2(paragraph) - already exists
  w.u8(static_cast<uint8_t>(OpCode::AddChild)); w.varint(2);
  // END_GROUP
  w.u8(static_cast<uint8_t>(OpCode::EndGroup));
  // SET_META key=5(title) value=14(main)
  w.u8(static_cast<uint8_t>(OpCode::SetMeta)); w.varint(5); w.varint(14);
  // SNAPSHOT label=13(root)
  w.u8(static_cast<uint8_t>(OpCode::Snapshot)); w.varint(13);
  return w.take();
}

// Journal seed: DROP_NODE then re-examine.
static std::vector<uint8_t> journal_drop_node() {
  ByteWriter w;
  const uint32_t N = 4;
  w.varint(N);
  // NEW_NODE kind=1(section) name=1(section)
  w.u8(static_cast<uint8_t>(OpCode::NewNode)); w.varint(1); w.varint(1);
  // SET_FIELD section.order = 7
  w.u8(static_cast<uint8_t>(OpCode::SetField)); w.varint(0); w.varint(2); w.svarint(7);
  // SNAPSHOT
  w.u8(static_cast<uint8_t>(OpCode::Snapshot)); w.varint(13);
  // DROP_NODE 0
  w.u8(static_cast<uint8_t>(OpCode::DropNode)); w.varint(0);
  return w.take();
}

// Journal seed: link_ref cross-node.
static std::vector<uint8_t> journal_link_ref() {
  ByteWriter w;
  const uint32_t N = 4;
  w.varint(N);
  // NEW_NODE kind=3(link) name=3(link)
  w.u8(static_cast<uint8_t>(OpCode::NewNode)); w.varint(3); w.varint(3);
  // SET_FIELD link.href(Atom) = atom 9
  w.u8(static_cast<uint8_t>(OpCode::SetField)); w.varint(0); w.varint(0); w.varint(9);
  // LINK_REF src=0 field=1(target,Ref) target_name=0(document)
  w.u8(static_cast<uint8_t>(OpCode::LinkRef)); w.varint(0); w.varint(1); w.varint(0);
  // NOP
  w.u8(static_cast<uint8_t>(OpCode::Nop));
  return w.take();
}

int main(int argc, char** argv) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: gen_corpus <container_dir> <journal_dir>\n");
    return 2;
  }
  std::string cdir = argv[1];
  std::string jdir = argv[2];
  bool ok = true;

  std::printf("Container seeds -> %s\n", cdir.c_str());
  ok &= emit(cdir, "seed_minimal.svm",   sample_container_minimal());
  ok &= emit(cdir, "seed_groups.svm",    sample_container_groups());
  ok &= emit(cdir, "seed_rich.svm",      sample_container_rich());
  ok &= emit(cdir, "seed_fwdref.svm",    container_forward_ref_safe());
  ok &= emit(cdir, "seed_xref.svm",      container_with_xref());

  std::printf("Journal seeds -> %s\n", jdir.c_str());
  ok &= emit(jdir, "seed_basic.jrnl",    sample_journal_basic());
  ok &= emit(jdir, "seed_groups.jrnl",   journal_group_existing());
  ok &= emit(jdir, "seed_multiop.jrnl",  journal_multiop());
  ok &= emit(jdir, "seed_drop.jrnl",     journal_drop_node());
  ok &= emit(jdir, "seed_linkref.jrnl",  journal_link_ref());

  return ok ? 0 : 1;
}
