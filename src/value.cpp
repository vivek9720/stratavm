#include "stratavm/value.hpp"

#include <cinttypes>
#include <cstdio>

namespace stratavm {

Status Value::decode(ByteReader& r, ValueType declared, const Limits& limits) {
  switch (declared) {
    case ValueType::Int: {
      std::int64_t v = 0;
      STRATAVM_RETURN_IF_ERROR(r.read_svarint(&v));
      *this = make_int(v);
      return Status::ok();
    }
    case ValueType::Float: {
      double v = 0;
      STRATAVM_RETURN_IF_ERROR(r.read_f64(&v));
      *this = make_float(v);
      return Status::ok();
    }
    case ValueType::Atom: {
      std::uint32_t v = 0;
      STRATAVM_RETURN_IF_ERROR(r.read_varint_u32(&v));
      *this = make_atom(v);
      return Status::ok();
    }
    case ValueType::Ref: {
      std::uint32_t v = 0;
      STRATAVM_RETURN_IF_ERROR(r.read_varint_u32(&v));
      *this = make_ref(v);
      return Status::ok();
    }
    case ValueType::List: {
      std::uint32_t n = 0;
      STRATAVM_RETURN_IF_ERROR(r.read_varint_u32(&n));
      if (n > limits.max_list_len)
        return fail(Code::LimitExceeded, "list too long");
      std::vector<NodeId> items;
      items.reserve(n);
      for (std::uint32_t i = 0; i < n; ++i) {
        std::uint32_t ref = 0;
        STRATAVM_RETURN_IF_ERROR(r.read_varint_u32(&ref));
        items.push_back(ref);
      }
      *this = make_list(std::move(items));
      return Status::ok();
    }
    default:
      return fail(Code::BadValue, "decode of invalid value type");
  }
}

std::string Value::to_string(const AtomPool& atoms) const {
  char buf[64];
  switch (type_) {
    case ValueType::Int:
      std::snprintf(buf, sizeof(buf), "%" PRId64, i_);
      return std::string("int(") + buf + ")";
    case ValueType::Float:
      std::snprintf(buf, sizeof(buf), "%g", f_);
      return std::string("float(") + buf + ")";
    case ValueType::Atom: {
      const std::string* s = atoms.get(static_cast<AtomId>(u_));
      return std::string("atom(") + (s ? *s : "?") + ")";
    }
    case ValueType::Ref:
      std::snprintf(buf, sizeof(buf), "%" PRIu64, u_);
      return std::string("ref(#") + buf + ")";
    case ValueType::List: {
      std::string out = "list[";
      for (std::size_t i = 0; i < list_.size(); ++i) {
        if (i) out += ',';
        std::snprintf(buf, sizeof(buf), "#%u", list_[i]);
        out += buf;
      }
      out += ']';
      return out;
    }
    default:
      return "invalid";
  }
}

}  // namespace stratavm
