#include "stratavm/samples.hpp"

#include "stratavm/builder.hpp"
#include "stratavm/byte_writer.hpp"
#include "stratavm/env.hpp"

namespace stratavm {

namespace {

// Field indices within each kind, mirroring env.cpp.
enum DocField { kDocTitle = 0, kDocBody = 1, kDocOrder = 2 };
enum SecField { kSecTitle = 0, kSecParent = 1, kSecOrder = 2 };
enum ParField { kParBody = 0 };

struct Ids {
  // atoms
  AtomId document, section, paragraph, link, image;
  AtomId title, body, order, parent, href, target, width, height;
  AtomId root, main, intro, appendix, figure;
  // kinds
  KindId k_document, k_section, k_paragraph, k_link, k_image;
};

// Intern atoms and define kinds exactly as build_default_env() does so the same
// numeric ids are valid in both the embedded and default schemas.
Ids apply_default_schema(ContainerBuilder& b) {
  Ids id;
  id.document = b.atom("document");
  id.section = b.atom("section");
  id.paragraph = b.atom("paragraph");
  id.link = b.atom("link");
  id.image = b.atom("image");
  id.title = b.atom("title");
  id.body = b.atom("body");
  id.order = b.atom("order");
  id.parent = b.atom("parent");
  id.href = b.atom("href");
  id.target = b.atom("target");
  id.width = b.atom("width");
  id.height = b.atom("height");
  id.root = b.atom("root");
  id.main = b.atom("main");
  id.intro = b.atom("intro");
  id.appendix = b.atom("appendix");
  id.figure = b.atom("figure");

  auto fd = [](AtomId name, ValueType t, std::uint8_t flags) {
    FieldDef f;
    f.name = name;
    f.type = t;
    f.flags = flags;
    return f;
  };
  id.k_document = b.define_kind(
      id.document, {fd(id.title, ValueType::Atom, kFieldIndexed),
                    fd(id.body, ValueType::Ref, kFieldNone),
                    fd(id.order, ValueType::Int, kFieldNone)});
  id.k_section = b.define_kind(
      id.section, {fd(id.title, ValueType::Atom, kFieldIndexed),
                   fd(id.parent, ValueType::Ref, kFieldNone),
                   fd(id.order, ValueType::Int, kFieldNone)});
  id.k_paragraph =
      b.define_kind(id.paragraph, {fd(id.body, ValueType::Atom, kFieldNone)});
  id.k_link = b.define_kind(
      id.link, {fd(id.href, ValueType::Atom, kFieldNone),
                fd(id.target, ValueType::Ref, kFieldNone)});
  id.k_image = b.define_kind(
      id.image, {fd(id.width, ValueType::Int, kFieldNone),
                 fd(id.height, ValueType::Int, kFieldNone)});
  return id;
}

}  // namespace

std::vector<std::uint8_t> sample_container_minimal() {
  ContainerBuilder b;
  Ids id = apply_default_schema(b);

  // Heap: a single document node id 0.
  NodeId doc = b.heap_node(id.k_document, id.root);
  b.heap_atom(doc, kDocTitle, id.main);
  b.heap_int(doc, kDocOrder, 1);

  // Journal: create a section, populate it, snapshot.
  NodeId next = 1;
  b.op_new_node(id.k_section, id.intro);
  NodeId sec = next++;
  b.op_set_atom(sec, kSecTitle, id.intro);
  b.op_set_int(sec, kSecOrder, 1);
  b.op_set_meta(id.title, id.main);
  b.op_snapshot(id.main);
  return b.serialize();
}

std::vector<std::uint8_t> sample_container_groups() {
  ContainerBuilder b;
  Ids id = apply_default_schema(b);

  NodeId doc = b.heap_node(id.k_document, id.root);
  b.heap_atom(doc, kDocTitle, id.main);

  // Define two children first, then attach them with a group. Because the names
  // already resolve, END_GROUP creates no placeholders - the safe path.
  NodeId next = 1;
  b.op_new_node(id.k_paragraph, id.intro);
  NodeId p1 = next++;
  b.op_set_atom(p1, kParBody, id.body);
  b.op_new_node(id.k_paragraph, id.appendix);
  NodeId p2 = next++;
  b.op_set_atom(p2, kParBody, id.body);

  b.op_begin_group(id.root, /*reserve_hint=*/4);
  b.op_add_child(id.intro);
  b.op_add_child(id.appendix);
  b.op_end_group();
  b.op_snapshot(id.main);
  (void)p2;
  return b.serialize();
}

std::vector<std::uint8_t> sample_container_rich() {
  ContainerBuilder b;
  Ids id = apply_default_schema(b);

  NodeId doc = b.heap_node(id.k_document, id.root);
  b.heap_atom(doc, kDocTitle, id.main);
  b.heap_int(doc, kDocOrder, 1);
  NodeId img = b.heap_node(id.k_image, id.figure);
  b.heap_int(img, 0, 640);
  b.heap_int(img, 1, 480);

  NodeId next = 2;
  b.op_new_node(id.k_section, id.intro);
  NodeId sec = next++;
  b.op_set_atom(sec, kSecTitle, id.intro);
  b.op_set_ref(sec, kSecParent, doc);
  b.op_new_node(id.k_link, id.appendix);
  NodeId lnk = next++;
  b.op_set_atom(lnk, 0, id.href);
  b.op_link_ref(lnk, 1, id.root);  // target -> document by name
  b.op_snapshot(id.intro);
  b.op_drop_node(lnk);
  b.op_snapshot(id.appendix);

  // Emit a cross-reference index and a metadata section too.
  b.xref_entry(id.root, doc);
  b.xref_entry(id.figure, img);
  b.meta_entry(id.title, id.main);
  return b.serialize();
}

// --- Bare journal payloads --------------------------------------------------

std::vector<std::uint8_t> sample_journal_basic() {
  // Hand-built against the default environment ids (see env.cpp ordering).
  ByteWriter w;
  const std::uint32_t op_count = 5;
  w.varint(op_count);

  // NEW_NODE document "root" -> node 0
  w.u8(static_cast<std::uint8_t>(OpCode::NewNode));
  w.varint(kKindDocument);
  w.varint(kAtomDocument);  // any valid atom id as the node name

  // SET_FIELD node0 title(Atom) = atom "section"
  w.u8(static_cast<std::uint8_t>(OpCode::SetField));
  w.varint(0);
  w.varint(kDocTitle);
  w.varint(kAtomSection);

  // NEW_NODE section "paragraph" -> node 1
  w.u8(static_cast<std::uint8_t>(OpCode::NewNode));
  w.varint(kKindSection);
  w.varint(kAtomParagraph);

  // SNAPSHOT label atom "image"
  w.u8(static_cast<std::uint8_t>(OpCode::Snapshot));
  w.varint(kAtomImage);

  // NOP
  w.u8(static_cast<std::uint8_t>(OpCode::Nop));
  return w.take();
}

std::vector<std::uint8_t> sample_journal_groups() {
  ByteWriter w;
  const std::uint32_t op_count = 6;
  w.varint(op_count);

  // NEW_NODE document "document" -> node 0 (named atom id 0)
  w.u8(static_cast<std::uint8_t>(OpCode::NewNode));
  w.varint(kKindDocument);
  w.varint(kAtomDocument);

  // NEW_NODE paragraph "section" -> node 1 (named atom id 1)
  w.u8(static_cast<std::uint8_t>(OpCode::NewNode));
  w.varint(kKindParagraph);
  w.varint(kAtomSection);

  // BEGIN_GROUP owner="document" hint=2
  w.u8(static_cast<std::uint8_t>(OpCode::BeginGroup));
  w.varint(kAtomDocument);
  w.varint(2);

  // ADD_CHILD "section" (already defined - resolves backward, no placeholder)
  w.u8(static_cast<std::uint8_t>(OpCode::AddChild));
  w.varint(kAtomSection);

  // END_GROUP
  w.u8(static_cast<std::uint8_t>(OpCode::EndGroup));

  // NOP
  w.u8(static_cast<std::uint8_t>(OpCode::Nop));
  return w.take();
}

}  // namespace stratavm
