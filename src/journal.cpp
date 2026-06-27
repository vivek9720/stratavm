#include "stratavm/journal.hpp"

namespace stratavm {

const char* op_name(OpCode op) {
  switch (op) {
    case OpCode::NewNode: return "NEW_NODE";
    case OpCode::SetField: return "SET_FIELD";
    case OpCode::BeginGroup: return "BEGIN_GROUP";
    case OpCode::EndGroup: return "END_GROUP";
    case OpCode::AddChild: return "ADD_CHILD";
    case OpCode::LinkRef: return "LINK_REF";
    case OpCode::DropNode: return "DROP_NODE";
    case OpCode::Snapshot: return "SNAPSHOT";
    case OpCode::SetMeta: return "SET_META";
    case OpCode::Nop: return "NOP";
  }
  return "??";
}

bool is_valid_opcode(std::uint8_t raw) {
  switch (static_cast<OpCode>(raw)) {
    case OpCode::NewNode:
    case OpCode::SetField:
    case OpCode::BeginGroup:
    case OpCode::EndGroup:
    case OpCode::AddChild:
    case OpCode::LinkRef:
    case OpCode::DropNode:
    case OpCode::Snapshot:
    case OpCode::SetMeta:
    case OpCode::Nop:
      return true;
    default:
      return false;
  }
}

OpArity op_arity(OpCode op) {
  switch (op) {
    case OpCode::NewNode: return {2, false};
    case OpCode::SetField: return {2, true};
    case OpCode::BeginGroup: return {2, false};
    case OpCode::EndGroup: return {0, false};
    case OpCode::AddChild: return {1, false};
    case OpCode::LinkRef: return {3, false};
    case OpCode::DropNode: return {1, false};
    case OpCode::Snapshot: return {1, false};
    case OpCode::SetMeta: return {2, false};
    case OpCode::Nop: return {0, false};
  }
  return {0, false};
}

}  // namespace stratavm
