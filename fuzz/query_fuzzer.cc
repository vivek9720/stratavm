// query_fuzzer.cc - fuzz the query engine parser and evaluator.
//
// Seeds the VM with a minimal default environment (same atoms / schema as
// the other harnesses) and then interprets the fuzz input as a
// NUL-terminated query string fed to QueryEngine::execute(). An empty or
// all-whitespace input is a valid, stable no-op.
//
// The harness exercises:
//   - The query lexer (token recognition, escape handling, unknown characters)
//   - The recursive-descent parser (deeply nested parens, truncated input)
//   - The evaluator (all predicate types, boolean combinations, dead nodes)
//
// Crashes here mean the query engine mishandles adversarial input, not the
// container format itself. That is a separate, complementary bug surface.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "stratavm/atom_pool.hpp"
#include "stratavm/env.hpp"
#include "stratavm/format.hpp"
#include "stratavm/node.hpp"
#include "stratavm/query.hpp"
#include "stratavm/schema.hpp"

// Persistent environment so LLVMFuzzerInitialize only runs once.
static stratavm::AtomPool* g_atoms = nullptr;
static stratavm::Schema*   g_schema = nullptr;
static std::vector<stratavm::Node>* g_nodes = nullptr;

extern "C" int LLVMFuzzerInitialize(int* /*argc*/, char*** /*argv*/) {
  g_atoms  = new stratavm::AtomPool;
  g_schema = new stratavm::Schema;
  stratavm::build_default_env(g_atoms, g_schema);

  // Build a small node graph: one document node and one paragraph node.
  g_nodes = new std::vector<stratavm::Node>;

  stratavm::Node doc;
  doc.id = 0; doc.kind = 0; doc.name = g_atoms->find("root");
  doc.alive = true; doc.placeholder = false; doc.generation = 0;
  doc.set_field(0, stratavm::Value::make_atom(g_atoms->find("main")));
  doc.set_field(2, stratavm::Value::make_int(1));
  g_nodes->push_back(std::move(doc));

  stratavm::Node para;
  para.id = 1; para.kind = 2; para.name = g_atoms->find("intro");
  para.alive = true; para.placeholder = false; para.generation = 0;
  para.set_field(0, stratavm::Value::make_atom(g_atoms->find("body")));
  para.refs.push_back(0);
  g_nodes->push_back(std::move(para));

  stratavm::Node dead;
  dead.id = 2; dead.kind = 1; dead.name = g_atoms->find("appendix");
  dead.alive = false; dead.placeholder = false; dead.generation = 1;
  g_nodes->push_back(std::move(dead));

  return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size == 0) return 0;
  // Treat the fuzz input as a raw UTF-8 query string (no NUL terminator needed).
  std::string query_text(reinterpret_cast<const char*>(data), size);

  stratavm::QueryEngine engine(*g_atoms, *g_schema, *g_nodes);
  // execute() is safe to call on any string; parse errors are returned as
  // Status errors rather than crashes.
  stratavm::QueryResult result = engine.execute(query_text);
  (void)result;
  return 0;
}
