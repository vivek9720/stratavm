// atom_pool.hpp - the interned string table (the ATOM section).
//
// Every human-readable name in a container - kind names, field names, node
// labels, snapshot labels, metadata keys - is stored once in the atom pool and
// referenced elsewhere by a small integer (AtomId). This keeps the rest of the
// format compact and means name comparisons during replay are integer
// comparisons rather than string compares.
#ifndef STRATAVM_ATOM_POOL_HPP
#define STRATAVM_ATOM_POOL_HPP

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "stratavm/byte_reader.hpp"
#include "stratavm/format.hpp"
#include "stratavm/status.hpp"

namespace stratavm {

using AtomId = std::uint32_t;
constexpr AtomId kInvalidAtom = 0xFFFFFFFFu;

class AtomPool {
 public:
  AtomPool() = default;

  // Decode the ATOM section payload: varint count followed by that many
  // length-prefixed strings.
  Status decode(ByteReader& r, const Limits& limits);

  std::size_t size() const { return atoms_.size(); }
  bool empty() const { return atoms_.empty(); }

  // Bounds-checked lookup. Returns nullptr for an out-of-range id.
  const std::string* get(AtomId id) const {
    if (id >= atoms_.size()) return nullptr;
    return &atoms_[id];
  }

  // Returns the atom text or a placeholder; convenient for diagnostics.
  const std::string& text_or(AtomId id, const std::string& fallback) const {
    const std::string* s = get(id);
    return s ? *s : fallback;
  }

  // Reverse lookup by content (first match). Used by the replay machine to
  // resolve names quickly and by the encoder to dedupe.
  AtomId find(const std::string& s) const {
    auto it = index_.find(s);
    return it == index_.end() ? kInvalidAtom : it->second;
  }

  // Used by the in-memory builder/encoder path.
  AtomId intern(const std::string& s);

 private:
  std::vector<std::string> atoms_;
  std::unordered_map<std::string, AtomId> index_;
};

}  // namespace stratavm

#endif  // STRATAVM_ATOM_POOL_HPP
