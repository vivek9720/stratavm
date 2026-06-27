// byte_writer.hpp - the encoder side of the wire format.
//
// The writer is the inverse of ByteReader. It is used by the unit tests, the
// CLI `build` command and the seed-corpus generator to produce well-formed
// containers. Keeping the encoder in the shipped library (rather than a throw
// away script) guarantees the seeds and tests stay in lock-step with whatever
// the decoder expects.
#ifndef STRATAVM_BYTE_WRITER_HPP
#define STRATAVM_BYTE_WRITER_HPP

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace stratavm {

class ByteWriter {
 public:
  ByteWriter() = default;

  const std::vector<std::uint8_t>& bytes() const { return buf_; }
  std::vector<std::uint8_t> take() { return std::move(buf_); }
  std::size_t size() const { return buf_.size(); }

  void u8(std::uint8_t v) { buf_.push_back(v); }

  void u16(std::uint16_t v) {
    buf_.push_back(static_cast<std::uint8_t>(v & 0xFF));
    buf_.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
  }

  void u32(std::uint32_t v) {
    for (int i = 0; i < 4; ++i)
      buf_.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
  }

  void u64(std::uint64_t v) {
    for (int i = 0; i < 8; ++i)
      buf_.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
  }

  void f64(double d) {
    std::uint64_t bits;
    std::memcpy(&bits, &d, sizeof(bits));
    u64(bits);
  }

  void varint(std::uint64_t v) {
    while (v >= 0x80) {
      buf_.push_back(static_cast<std::uint8_t>((v & 0x7F) | 0x80));
      v >>= 7;
    }
    buf_.push_back(static_cast<std::uint8_t>(v));
  }

  void svarint(std::int64_t v) {
    std::uint64_t zz = (static_cast<std::uint64_t>(v) << 1) ^
                       static_cast<std::uint64_t>(v >> 63);
    varint(zz);
  }

  void raw(const std::string& s) {
    buf_.insert(buf_.end(), s.begin(), s.end());
  }

  void raw(const std::uint8_t* p, std::size_t n) {
    buf_.insert(buf_.end(), p, p + n);
  }

  void lp_string(const std::string& s) {
    varint(s.size());
    raw(s);
  }

  // Patch a previously reserved 4-byte little-endian slot. The section-table
  // builder reserves offset/length slots and back-patches them once the section
  // payload position is known.
  void patch_u32(std::size_t at, std::uint32_t v) {
    for (int i = 0; i < 4; ++i)
      buf_[at + i] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF);
  }

  // Append the contents of another writer (a finished section payload).
  void append(const ByteWriter& other) {
    buf_.insert(buf_.end(), other.buf_.begin(), other.buf_.end());
  }

 private:
  std::vector<std::uint8_t> buf_;
};

}  // namespace stratavm

#endif  // STRATAVM_BYTE_WRITER_HPP
