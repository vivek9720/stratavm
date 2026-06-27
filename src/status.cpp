#include "stratavm/status.hpp"

namespace stratavm {

const char* code_name(Code c) {
  switch (c) {
    case Code::Ok: return "ok";
    case Code::Truncated: return "truncated";
    case Code::BadMagic: return "bad_magic";
    case Code::UnsupportedVersion: return "unsupported_version";
    case Code::BadSectionTable: return "bad_section_table";
    case Code::DuplicateSection: return "duplicate_section";
    case Code::MissingSection: return "missing_section";
    case Code::BadAtomPool: return "bad_atom_pool";
    case Code::BadSchema: return "bad_schema";
    case Code::BadHeap: return "bad_heap";
    case Code::BadJournal: return "bad_journal";
    case Code::BadXref: return "bad_xref";
    case Code::BadValue: return "bad_value";
    case Code::BadOpcode: return "bad_opcode";
    case Code::NodeOutOfRange: return "node_out_of_range";
    case Code::AtomOutOfRange: return "atom_out_of_range";
    case Code::KindOutOfRange: return "kind_out_of_range";
    case Code::FieldOutOfRange: return "field_out_of_range";
    case Code::TypeMismatch: return "type_mismatch";
    case Code::GroupUnderflow: return "group_underflow";
    case Code::GroupOverflow: return "group_overflow";
    case Code::UnresolvedRef: return "unresolved_ref";
    case Code::InvariantViolation: return "invariant_violation";
    case Code::LimitExceeded: return "limit_exceeded";
    case Code::Internal: return "internal";
    case Code::BadChecksum: return "bad_checksum";
    case Code::NotFound: return "not_found";
    case Code::AlreadyExists: return "already_exists";
  }
  return "unknown";
}

std::string Status::to_string() const {
  if (is_ok()) return "ok";
  std::string out = code_name(code_);
  if (!detail_.empty()) {
    out += ": ";
    out += detail_;
  }
  return out;
}

}  // namespace stratavm
