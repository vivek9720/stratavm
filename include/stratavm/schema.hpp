// schema.hpp - the type table (the SCMA section).
//
// A schema declares the "kinds" of node a container may contain. Each kind has a
// name and an ordered list of typed fields. During heap decoding and journal
// replay the schema tells the machine how to interpret a field value (is field 2
// of kind "edge" an Int, an Atom or a Ref?) and lets the validator check that a
// node only carries fields its kind declares.
#ifndef STRATAVM_SCHEMA_HPP
#define STRATAVM_SCHEMA_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "stratavm/atom_pool.hpp"
#include "stratavm/byte_reader.hpp"
#include "stratavm/format.hpp"
#include "stratavm/status.hpp"

namespace stratavm {

using KindId = std::uint32_t;
using FieldId = std::uint32_t;
constexpr KindId kInvalidKind = 0xFFFFFFFFu;

// Field-level flags carried in the schema.
enum FieldFlags : std::uint8_t {
  kFieldNone = 0,
  kFieldRequired = 1u << 0,
  kFieldIndexed = 1u << 1,  // participates in the XREF name index
};

struct FieldDef {
  AtomId name = kInvalidAtom;
  ValueType type = ValueType::Invalid;
  std::uint8_t flags = kFieldNone;

  bool is_required() const { return (flags & kFieldRequired) != 0; }
  bool is_indexed() const { return (flags & kFieldIndexed) != 0; }
};

struct KindDef {
  AtomId name = kInvalidAtom;
  std::vector<FieldDef> fields;

  const FieldDef* field(FieldId id) const {
    if (id >= fields.size()) return nullptr;
    return &fields[id];
  }
};

class Schema {
 public:
  Schema() = default;

  // Decode the SCMA section payload. `atoms` is needed only to bound-check name
  // references; the schema stores ids, not strings.
  Status decode(ByteReader& r, const AtomPool& atoms, const Limits& limits);

  std::size_t kind_count() const { return kinds_.size(); }
  bool empty() const { return kinds_.empty(); }

  const KindDef* kind(KindId id) const {
    if (id >= kinds_.size()) return nullptr;
    return &kinds_[id];
  }

  // Resolve a field type for (kind, field). Returns Invalid when either index is
  // out of range so callers get a single clear failure point.
  ValueType field_type(KindId kind_id, FieldId field_id) const {
    const KindDef* k = kind(kind_id);
    if (!k) return ValueType::Invalid;
    const FieldDef* f = k->field(field_id);
    if (!f) return ValueType::Invalid;
    return f->type;
  }

  // Used by the in-memory builder.
  KindId add_kind(const KindDef& k);

 private:
  std::vector<KindDef> kinds_;
};

// True if `t` is a recognised on-disk value type byte.
bool is_valid_value_type(std::uint8_t raw);

}  // namespace stratavm

#endif  // STRATAVM_SCHEMA_HPP
