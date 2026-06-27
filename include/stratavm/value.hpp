// value.hpp - a tagged field value plus its byte-level decoder.
//
// Field values are small and uniform, so rather than pulling in std::variant we
// use an explicit tag with inline storage for scalars and a vector for the one
// compound type (List). The decoder is schema-driven: the caller supplies the
// expected ValueType (from the kind's FieldDef) and decode() reads exactly the
// bytes that type implies.
#ifndef STRATAVM_VALUE_HPP
#define STRATAVM_VALUE_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "stratavm/atom_pool.hpp"
#include "stratavm/byte_reader.hpp"
#include "stratavm/format.hpp"
#include "stratavm/status.hpp"

namespace stratavm {

using NodeId = std::uint32_t;
constexpr NodeId kInvalidNode = 0xFFFFFFFFu;

class Value {
 public:
  Value() : type_(ValueType::Invalid), i_(0) {}

  static Value make_int(std::int64_t v) {
    Value x;
    x.type_ = ValueType::Int;
    x.i_ = v;
    return x;
  }
  static Value make_float(double v) {
    Value x;
    x.type_ = ValueType::Float;
    x.f_ = v;
    return x;
  }
  static Value make_atom(AtomId v) {
    Value x;
    x.type_ = ValueType::Atom;
    x.u_ = v;
    return x;
  }
  static Value make_ref(NodeId v) {
    Value x;
    x.type_ = ValueType::Ref;
    x.u_ = v;
    return x;
  }
  static Value make_list(std::vector<NodeId> items) {
    Value x;
    x.type_ = ValueType::List;
    x.list_ = std::move(items);
    return x;
  }

  ValueType type() const { return type_; }
  bool valid() const { return type_ != ValueType::Invalid; }

  std::int64_t as_int() const { return i_; }
  double as_float() const { return f_; }
  AtomId as_atom() const { return static_cast<AtomId>(u_); }
  NodeId as_ref() const { return static_cast<NodeId>(u_); }
  const std::vector<NodeId>& as_list() const { return list_; }
  std::vector<NodeId>& as_list() { return list_; }

  // Decode a value of the given declared type from `r`. List elements are node
  // references and may point forward (validated later during replay/validation).
  Status decode(ByteReader& r, ValueType declared, const Limits& limits);

  // Render for diagnostics / the CLI dump command.
  std::string to_string(const AtomPool& atoms) const;

 private:
  ValueType type_;
  union {
    std::int64_t i_;
    double f_;
    std::uint64_t u_;
  };
  std::vector<NodeId> list_;
};

}  // namespace stratavm

#endif  // STRATAVM_VALUE_HPP
