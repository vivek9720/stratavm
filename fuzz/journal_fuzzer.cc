// journal_fuzzer.cc - drives the journal replay VM with a fixed schema.
//
// This harness interprets the entire fuzz input as a bare journal payload: a
// varint op count followed by op bytes. A fixed "default environment" schema
// (the document model from env.hpp) is installed once at startup, and a single
// named root document node is seeded onto the heap. The journal is then replayed
// against that state.
//
// Because the schema is fixed and known, the fuzzer can explore journal sequences
// much more deeply than the container harness (which must also fuzz the schema
// itself). The group-construction / forward-reference path is especially
// reachable here because the seeds contain valid BEGIN_GROUP / END_GROUP
// sequences that the fuzzer can mutate by injecting ADD_CHILD names and varying
// the reserve_hint operand.
#include <cstdint>
#include <cstddef>
#include <vector>

#include "stratavm/atom_pool.hpp"
#include "stratavm/byte_reader.hpp"
#include "stratavm/env.hpp"
#include "stratavm/format.hpp"
#include "stratavm/heap.hpp"
#include "stratavm/replay_vm.hpp"
#include "stratavm/schema.hpp"

// The schema and atoms are static; they are initialised once and shared across
// fuzz runs. The atoms vector outlives the schema references because both are
// stored as static locals here.
static stratavm::AtomPool* g_atoms = nullptr;
static stratavm::Schema*   g_schema = nullptr;

// libFuzzer calls this once before the first run.
extern "C" int LLVMFuzzerInitialize(int* /*argc*/, char*** /*argv*/) {
  g_atoms  = new stratavm::AtomPool();
  g_schema = new stratavm::Schema();
  stratavm::build_default_env(g_atoms, g_schema);
  return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  using namespace stratavm;

  Limits limits;

  // Seed the VM with a single named "root" document node on the heap.
  HeapImage heap;
  {
    Node root;
    root.id = 0;
    root.kind = kKindDocument;
    root.name = static_cast<AtomId>(kAtomDocument); // atom id 0 = "document"
    root.alive = true;
    heap.nodes.push_back(root);
  }

  ReplayVM vm(*g_schema, *g_atoms, limits);
  vm.seed(std::move(heap));

  // Treat the full fuzz input as the journal section payload.
  ByteReader r(data, size);
  auto st = vm.run(r);
  (void)st;
  return 0;
}
