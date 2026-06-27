#include "stratavm/env.hpp"

namespace stratavm {

void build_default_env(AtomPool* atoms, Schema* schema) {
  // Kind-name atoms first so DefaultAtom/DefaultKind ids line up.
  const AtomId a_document = atoms->intern("document");
  const AtomId a_section = atoms->intern("section");
  const AtomId a_paragraph = atoms->intern("paragraph");
  const AtomId a_link = atoms->intern("link");
  const AtomId a_image = atoms->intern("image");

  // Field-name atoms.
  const AtomId a_title = atoms->intern("title");
  const AtomId a_body = atoms->intern("body");
  const AtomId a_order = atoms->intern("order");
  const AtomId a_parent = atoms->intern("parent");
  const AtomId a_href = atoms->intern("href");
  const AtomId a_target = atoms->intern("target");
  const AtomId a_width = atoms->intern("width");
  const AtomId a_height = atoms->intern("height");

  // A few extra names useful as node labels in seeds and the dictionary.
  atoms->intern("root");
  atoms->intern("main");
  atoms->intern("intro");
  atoms->intern("appendix");
  atoms->intern("figure");

  auto field = [](AtomId name, ValueType type, std::uint8_t flags) {
    FieldDef f;
    f.name = name;
    f.type = type;
    f.flags = flags;
    return f;
  };

  {
    KindDef k;
    k.name = a_document;
    k.fields.push_back(field(a_title, ValueType::Atom, kFieldIndexed));
    k.fields.push_back(field(a_body, ValueType::Ref, kFieldNone));
    k.fields.push_back(field(a_order, ValueType::Int, kFieldNone));
    schema->add_kind(k);  // kKindDocument
  }
  {
    KindDef k;
    k.name = a_section;
    k.fields.push_back(field(a_title, ValueType::Atom, kFieldIndexed));
    k.fields.push_back(field(a_parent, ValueType::Ref, kFieldNone));
    k.fields.push_back(field(a_order, ValueType::Int, kFieldNone));
    schema->add_kind(k);  // kKindSection
  }
  {
    KindDef k;
    k.name = a_paragraph;
    k.fields.push_back(field(a_body, ValueType::Atom, kFieldNone));
    schema->add_kind(k);  // kKindParagraph
  }
  {
    KindDef k;
    k.name = a_link;
    k.fields.push_back(field(a_href, ValueType::Atom, kFieldNone));
    k.fields.push_back(field(a_target, ValueType::Ref, kFieldNone));
    schema->add_kind(k);  // kKindLink
  }
  {
    KindDef k;
    k.name = a_image;
    k.fields.push_back(field(a_width, ValueType::Int, kFieldNone));
    k.fields.push_back(field(a_height, ValueType::Int, kFieldNone));
    schema->add_kind(k);  // kKindImage
  }
}

}  // namespace stratavm
