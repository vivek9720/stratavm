// loader.hpp - the top-level entry point that turns bytes into a graph.
//
// load() ties the whole pipeline together: parse the section directory, decode
// the atom pool, schema and initial heap, replay the journal, build the
// cross-reference index, reconcile the optional XREF section and validate the
// result. It is the function the container fuzz target drives, so it is written
// to surface every failure as a Status rather than aborting.
#ifndef STRATAVM_LOADER_HPP
#define STRATAVM_LOADER_HPP

#include <cstdint>
#include <memory>
#include <unordered_map>

#include "stratavm/atom_pool.hpp"
#include "stratavm/format.hpp"
#include "stratavm/replay_vm.hpp"
#include "stratavm/schema.hpp"
#include "stratavm/section.hpp"
#include "stratavm/status.hpp"
#include "stratavm/validator.hpp"
#include "stratavm/xref_index.hpp"

namespace stratavm {

struct LoadOptions {
  Limits limits;
  bool run_journal = true;   // replay the JRNL section after seeding the heap
  bool strict_validation = false;
};

// Everything produced by a successful (or partially successful) load. Owns the
// atom pool, schema and limits so the embedded VM's references stay valid. The
// type is deliberately non-movable; always hold it behind the unique_ptr that
// load() returns.
class LoadedContainer {
 public:
  explicit LoadedContainer(const Limits& l)
      : limits(l), vm(schema, atoms, limits) {}

  LoadedContainer(const LoadedContainer&) = delete;
  LoadedContainer& operator=(const LoadedContainer&) = delete;

  Limits limits;
  AtomPool atoms;
  Schema schema;
  SectionTable sections;
  ReplayVM vm;
  XrefIndex xref;
  ValidationReport report;
  std::unordered_map<AtomId, AtomId> section_meta;
  std::uint32_t xref_warnings = 0;
  bool journal_ran = false;
};

// Parse and reconstruct. Returns a populated LoadedContainer on success, or a
// Status describing the first structural failure encountered.
Result<std::unique_ptr<LoadedContainer>> load(const std::uint8_t* data,
                                              std::size_t size,
                                              const LoadOptions& options);

}  // namespace stratavm

#endif  // STRATAVM_LOADER_HPP
