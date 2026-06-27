#include "stratavm/section.hpp"

#include <cstring>

namespace stratavm {

Status SectionTable::decode(ByteReader& r, std::size_t file_size,
                            const Limits& limits) {
  // Magic.
  std::string magic;
  STRATAVM_RETURN_IF_ERROR(r.read_bytes(kMagicLen, &magic));
  if (std::memcmp(magic.data(), kMagic, kMagicLen) != 0)
    return fail(Code::BadMagic, "magic mismatch");

  STRATAVM_RETURN_IF_ERROR(r.read_u8(&header_.version_major));
  STRATAVM_RETURN_IF_ERROR(r.read_u8(&header_.version_minor));
  STRATAVM_RETURN_IF_ERROR(r.read_u16(&header_.flags));
  STRATAVM_RETURN_IF_ERROR(r.read_u32(&header_.section_count));

  // We accept any minor version of major 1; a future major version would change
  // the directory layout and is rejected up front.
  if (header_.version_major != kVersionMajor)
    return fail(Code::UnsupportedVersion, "major version not supported");
  if (header_.section_count > limits.max_sections)
    return fail(Code::LimitExceeded, "too many sections");

  entries_.clear();
  entries_.reserve(header_.section_count);

  for (std::uint32_t i = 0; i < header_.section_count; ++i) {
    SectionEntry e;
    std::uint8_t reserved = 0;
    STRATAVM_RETURN_IF_ERROR(r.read_u32(&e.tag));
    STRATAVM_RETURN_IF_ERROR(r.read_u8(&e.version));
    STRATAVM_RETURN_IF_ERROR(r.read_u8(&reserved));
    STRATAVM_RETURN_IF_ERROR(r.read_u16(&e.flags));
    STRATAVM_RETURN_IF_ERROR(r.read_u32(&e.offset));
    STRATAVM_RETURN_IF_ERROR(r.read_u32(&e.length));

    // The window must lie entirely within the file.
    if (e.offset > file_size || e.length > file_size - e.offset)
      return fail(Code::BadSectionTable, "section window out of range");

    // Reject a second directory entry for the same known tag; ambiguous which
    // one wins otherwise. Unknown tags may repeat (they are ignored anyway).
    if (e.kind() != SectionTag::Unknown) {
      for (const auto& prev : entries_) {
        if (prev.tag == e.tag)
          return fail(Code::DuplicateSection, "duplicate section tag");
      }
    }
    entries_.push_back(e);
  }
  return Status::ok();
}

const SectionEntry* SectionTable::find(SectionTag tag) const {
  for (const auto& e : entries_) {
    if (e.kind() == tag) return &e;
  }
  return nullptr;
}

}  // namespace stratavm
