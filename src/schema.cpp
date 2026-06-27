#include "stratavm/schema.hpp"

namespace stratavm {

bool is_valid_value_type(std::uint8_t raw) {
  switch (static_cast<ValueType>(raw)) {
    case ValueType::Int:
    case ValueType::Float:
    case ValueType::Atom:
    case ValueType::Ref:
    case ValueType::List:
      return true;
    default:
      return false;
  }
}

Status Schema::decode(ByteReader& r, const AtomPool& atoms, const Limits& limits) {
  std::uint32_t kind_count = 0;
  STRATAVM_RETURN_IF_ERROR(r.read_varint_u32(&kind_count));
  if (kind_count > limits.max_kinds)
    return fail(Code::LimitExceeded, "kind count too large");

  kinds_.clear();
  kinds_.reserve(kind_count);

  for (std::uint32_t ki = 0; ki < kind_count; ++ki) {
    KindDef kd;
    STRATAVM_RETURN_IF_ERROR(r.read_varint_u32(&kd.name));
    if (atoms.get(kd.name) == nullptr)
      return fail(Code::BadSchema, "kind name atom out of range");

    std::uint32_t field_count = 0;
    STRATAVM_RETURN_IF_ERROR(r.read_varint_u32(&field_count));
    if (field_count > limits.max_fields_per_kind)
      return fail(Code::LimitExceeded, "field count too large");
    kd.fields.reserve(field_count);

    for (std::uint32_t fi = 0; fi < field_count; ++fi) {
      FieldDef fd;
      STRATAVM_RETURN_IF_ERROR(r.read_varint_u32(&fd.name));
      if (atoms.get(fd.name) == nullptr)
        return fail(Code::BadSchema, "field name atom out of range");

      std::uint8_t type_raw = 0;
      STRATAVM_RETURN_IF_ERROR(r.read_u8(&type_raw));
      if (!is_valid_value_type(type_raw))
        return fail(Code::BadSchema, "unknown field type");
      fd.type = static_cast<ValueType>(type_raw);

      STRATAVM_RETURN_IF_ERROR(r.read_u8(&fd.flags));
      kd.fields.push_back(fd);
    }
    kinds_.push_back(std::move(kd));
  }
  return Status::ok();
}

KindId Schema::add_kind(const KindDef& k) {
  KindId id = static_cast<KindId>(kinds_.size());
  kinds_.push_back(k);
  return id;
}

}  // namespace stratavm
