// validator.hpp - post-replay invariant checks over the reconstructed graph.
//
// Validation is intentionally separate from replay. The VM's job is to faithfully
// apply the journal; the validator's job is to report whether the *result* is a
// well-formed graph. It walks every live node and checks: required fields are
// present, references point at live in-range nodes, and no placeholder was left
// unfilled (a forward reference that nothing ever defined).
#ifndef STRATAVM_VALIDATOR_HPP
#define STRATAVM_VALIDATOR_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "stratavm/atom_pool.hpp"
#include "stratavm/replay_vm.hpp"
#include "stratavm/schema.hpp"
#include "stratavm/status.hpp"

namespace stratavm {

struct ValidationIssue {
  Code code;
  NodeId node;
  std::string detail;
};

struct ValidationReport {
  std::vector<ValidationIssue> issues;
  std::uint32_t live_nodes = 0;
  std::uint32_t dangling_refs = 0;
  std::uint32_t unfilled_placeholders = 0;
  std::uint32_t missing_required = 0;

  bool ok() const { return issues.empty(); }
};

// Run all structural checks. `strict` decides whether soft problems (unfilled
// placeholders, dangling refs) are recorded as issues or merely counted.
ValidationReport validate_graph(const ReplayVM& vm, const Schema& schema,
                                const AtomPool& atoms, bool strict);

}  // namespace stratavm

#endif  // STRATAVM_VALIDATOR_HPP
