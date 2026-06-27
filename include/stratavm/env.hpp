// env.hpp - the canonical schema used by the journal harness and samples.
//
// The container format carries its own schema, but the journal-only fuzz target
// and the sample/seed generators need a fixed, well-known schema to interpret
// raw journal bytes against. build_default_env() populates an AtomPool and
// Schema with a small document-model vocabulary whose atom and kind ids are
// stable, so seeds and the dictionary can refer to them by number.
#ifndef STRATAVM_ENV_HPP
#define STRATAVM_ENV_HPP

#include "stratavm/atom_pool.hpp"
#include "stratavm/schema.hpp"

namespace stratavm {

// Stable kind ids in the default environment.
enum DefaultKind : KindId {
  kKindDocument = 0,
  kKindSection = 1,
  kKindParagraph = 2,
  kKindLink = 3,
  kKindImage = 4,
};

// A handful of stable atom ids callers can rely on. The remaining names are
// interned after these in build_default_env().
enum DefaultAtom : AtomId {
  kAtomDocument = 0,
  kAtomSection = 1,
  kAtomParagraph = 2,
  kAtomLink = 3,
  kAtomImage = 4,
};

// Populate `atoms` and `schema` with the default document model. Safe to call on
// freshly constructed objects.
void build_default_env(AtomPool* atoms, Schema* schema);

}  // namespace stratavm

#endif  // STRATAVM_ENV_HPP
