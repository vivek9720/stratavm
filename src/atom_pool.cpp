#include "stratavm/atom_pool.hpp"

namespace stratavm {

Status AtomPool::decode(ByteReader& r, const Limits& limits) {
  std::uint32_t count = 0;
  STRATAVM_RETURN_IF_ERROR(r.read_varint_u32(&count));
  if (count > limits.max_atoms)
    return fail(Code::LimitExceeded, "atom count too large");

  atoms_.clear();
  index_.clear();
  atoms_.reserve(count);

  for (std::uint32_t i = 0; i < count; ++i) {
    std::string s;
    Status st = r.read_lp_string(&s, limits.max_atom_len);
    if (st.is_error())
      return fail(Code::BadAtomPool, "atom " + std::to_string(i) + ": " + st.detail());
    // The reverse index keeps the first occurrence of a given string. Later
    // duplicates still occupy their own slot (ids are positional) but resolve
    // to the earliest id, which matches producer expectations.
    if (index_.find(s) == index_.end())
      index_.emplace(s, static_cast<AtomId>(i));
    atoms_.push_back(std::move(s));
  }
  return Status::ok();
}

AtomId AtomPool::intern(const std::string& s) {
  auto it = index_.find(s);
  if (it != index_.end()) return it->second;
  AtomId id = static_cast<AtomId>(atoms_.size());
  atoms_.push_back(s);
  index_.emplace(s, id);
  return id;
}

}  // namespace stratavm
