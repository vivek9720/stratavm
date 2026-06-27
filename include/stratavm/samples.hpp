// samples.hpp - canonical valid containers and journal streams.
//
// These builders produce the well-formed inputs used as fuzzing seeds, unit-test
// fixtures and CLI demos. They all use the same document-model schema as
// env.hpp, so a journal stream produced here can be replayed against the default
// environment by the journal fuzz target.
//
// None of the samples exercise the forward-reference-heavy group path that the
// known bug needs: seeds are meant to reach deep code quickly while staying on
// the safe side of the invariant the bug violates.
#ifndef STRATAVM_SAMPLES_HPP
#define STRATAVM_SAMPLES_HPP

#include <cstdint>
#include <vector>

namespace stratavm {

// Full .svm containers.
std::vector<std::uint8_t> sample_container_minimal();
std::vector<std::uint8_t> sample_container_groups();
std::vector<std::uint8_t> sample_container_rich();

// Bare journal payloads (varint op_count + ops) for the journal fuzz target,
// expressed against the default environment's kind/atom ids.
std::vector<std::uint8_t> sample_journal_basic();
std::vector<std::uint8_t> sample_journal_groups();

}  // namespace stratavm

#endif  // STRATAVM_SAMPLES_HPP
