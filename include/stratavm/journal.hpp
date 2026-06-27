// journal.hpp - opcode metadata for the JRNL section.
//
// The journal is a flat stream of opcodes. Operands are decoded inline by the
// replay machine because some of them (notably SET_FIELD values) depend on the
// live type of the target node, which is only known once earlier ops have run.
// This header exposes naming/arity helpers used by the CLI tracer and the unit
// tests; the actual decode+execute loop lives in replay_vm.cpp.
#ifndef STRATAVM_JOURNAL_HPP
#define STRATAVM_JOURNAL_HPP

#include "stratavm/format.hpp"

namespace stratavm {

// Human-readable mnemonic for an opcode.
const char* op_name(OpCode op);

// True if `raw` is a defined opcode byte.
bool is_valid_opcode(std::uint8_t raw);

// A coarse description of how many integer/atom operands an opcode consumes,
// used by the CLI to print a structural trace and by tests as documentation.
struct OpArity {
  std::uint8_t varints;  // plain varint operands
  bool has_value;        // trailing schema-typed value (SET_FIELD)
};

OpArity op_arity(OpCode op);

}  // namespace stratavm

#endif  // STRATAVM_JOURNAL_HPP
