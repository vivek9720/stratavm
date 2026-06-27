#include "test_runner.hpp"
#include "stratavm/atom_pool.hpp"
#include "stratavm/byte_writer.hpp"
#include "stratavm/schema.hpp"

using namespace stratavm;
using namespace stratavm::test;
using stratavm::Code;

static std::vector<uint8_t> encode_atoms(const std::vector<std::string>& names) {
  ByteWriter w;
  w.varint(names.size());
  for (const auto& s : names) w.lp_string(s);
  return w.take();
}

STEST(atom_pool_basic) {
  auto bytes = encode_atoms({"foo", "bar", "baz"});
  ByteReader r(bytes.data(), bytes.size());
  AtomPool pool;
  Limits lim;
  REQUIRE_OK(pool.decode(r, lim));
  REQUIRE_EQ(pool.size(), 3u);
  REQUIRE_EQ(*pool.get(0), std::string("foo"));
  REQUIRE_EQ(*pool.get(1), std::string("bar"));
  REQUIRE_EQ(pool.find("baz"), 2u);
  REQUIRE_EQ(pool.find("missing"), kInvalidAtom);
  REQUIRE(pool.get(3) == nullptr);
}

STEST(atom_pool_empty) {
  auto bytes = encode_atoms({});
  ByteReader r(bytes.data(), bytes.size());
  AtomPool pool;
  Limits lim;
  REQUIRE_OK(pool.decode(r, lim));
  REQUIRE_EQ(pool.size(), 0u);
  REQUIRE(pool.empty());
}

STEST(atom_pool_intern) {
  AtomPool pool;
  AtomId id1 = pool.intern("hello");
  AtomId id2 = pool.intern("world");
  AtomId id3 = pool.intern("hello");
  REQUIRE_EQ(id1, 0u);
  REQUIRE_EQ(id2, 1u);
  REQUIRE_EQ(id1, id3);
}

STEST(atom_pool_limit) {
  ByteWriter w;
  w.varint(5); // claim 5 atoms
  // only write 2 -> truncation
  w.lp_string("a");
  w.lp_string("b");
  auto bytes = w.take();
  ByteReader r(bytes.data(), bytes.size());
  AtomPool pool;
  Limits lim;
  auto st = pool.decode(r, lim);
  REQUIRE_ERR(st, Code::BadAtomPool);
}

STEST(schema_decode_basic) {
  AtomPool atoms;
  AtomId a_node = atoms.intern("node");
  AtomId a_id = atoms.intern("id");
  AtomId a_tag = atoms.intern("tag");

  ByteWriter w;
  w.varint(1); // 1 kind
  w.varint(a_node); // kind name
  w.varint(2); // 2 fields
    w.varint(a_id);  w.u8(static_cast<uint8_t>(ValueType::Int));   w.u8(kFieldRequired);
    w.varint(a_tag); w.u8(static_cast<uint8_t>(ValueType::Atom));  w.u8(kFieldNone);
  auto bytes = w.take();

  ByteReader r(bytes.data(), bytes.size());
  Schema schema;
  Limits lim;
  REQUIRE_OK(schema.decode(r, atoms, lim));
  REQUIRE_EQ(schema.kind_count(), 1u);
  const KindDef* k = schema.kind(0);
  REQUIRE(k != nullptr);
  REQUIRE_EQ(k->fields.size(), 2u);
  REQUIRE_EQ(k->fields[0].type, ValueType::Int);
  REQUIRE(k->fields[0].is_required());
  REQUIRE_EQ(k->fields[1].type, ValueType::Atom);
}

STEST(schema_bad_field_type) {
  AtomPool atoms;
  AtomId a_x = atoms.intern("x");
  AtomId a_f = atoms.intern("f");
  ByteWriter w;
  w.varint(1);
  w.varint(a_x);
  w.varint(1);
    w.varint(a_f); w.u8(0xFF); w.u8(0); // invalid type
  auto bytes = w.take();
  ByteReader r(bytes.data(), bytes.size());
  Schema schema;
  Limits lim;
  REQUIRE_ERR(schema.decode(r, atoms, lim), Code::BadSchema);
}

int main(int argc, char** argv) { return run_all(argc, argv); }
