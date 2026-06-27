// section.hpp - the file header and section directory.
//
// After the 16-byte fixed header comes a directory of fixed-size entries, one
// per section. Each entry names a tag and points at an absolute [offset,length)
// window in the file. The loader uses this directory to carve sub-readers for
// each section, so a section can appear in any order in the file and an unknown
// tag is simply skipped.
#ifndef STRATAVM_SECTION_HPP
#define STRATAVM_SECTION_HPP

#include <cstdint>
#include <vector>

#include "stratavm/byte_reader.hpp"
#include "stratavm/format.hpp"
#include "stratavm/status.hpp"

namespace stratavm {

struct FileHeader {
  std::uint8_t version_major = 0;
  std::uint8_t version_minor = 0;
  std::uint16_t flags = 0;
  std::uint32_t section_count = 0;
};

struct SectionEntry {
  std::uint32_t tag = 0;
  std::uint8_t version = 0;
  std::uint16_t flags = 0;
  std::uint32_t offset = 0;
  std::uint32_t length = 0;

  SectionTag kind() const { return static_cast<SectionTag>(tag); }
};

class SectionTable {
 public:
  SectionTable() = default;

  // Parse the header magic, version and the directory. `r` must be positioned at
  // the start of the file. Validates that each section window lies within the
  // file and that no tag is declared twice.
  Status decode(ByteReader& r, std::size_t file_size, const Limits& limits);

  const FileHeader& header() const { return header_; }
  const std::vector<SectionEntry>& entries() const { return entries_; }

  // Find a section by tag. Returns nullptr if absent.
  const SectionEntry* find(SectionTag tag) const;

 private:
  FileHeader header_;
  std::vector<SectionEntry> entries_;
};

// Size of a single directory entry on disk.
constexpr std::size_t kSectionEntrySize = 4 + 1 + 1 + 2 + 4 + 4;  // 16 bytes
// Size of the fixed file header (magic + version + flags + count).
constexpr std::size_t kFileHeaderSize = kMagicLen + 1 + 1 + 2 + 4;  // 16 bytes

}  // namespace stratavm

#endif  // STRATAVM_SECTION_HPP
