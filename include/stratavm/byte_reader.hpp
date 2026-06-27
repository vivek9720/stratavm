// byte_reader.hpp - a bounds-checked cursor over an immutable byte span.
//
// Every primitive the container format uses (fixed-width little-endian ints,
// unsigned LEB128 varints, zig-zag signed varints, IEEE-754 doubles and raw
// byte runs) is decoded through this single type. Decoders never index the
// underlying buffer directly, which is what keeps the non-buggy paths free of
// out-of-bounds access regardless of how hostile the input is.
#ifndef STRATAVM_BYTE_READER_HPP
#define STRATAVM_BYTE_READER_HPP

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include "stratavm/status.hpp"

namespace stratavm {

class ByteReader {
 public:
  ByteReader() : data_(nullptr), size_(0), pos_(0) {}
  ByteReader(const std::uint8_t* data, std::size_t size)
      : data_(data), size_(size), pos_(0) {}

  ByteReader(const ByteReader&) = default;

  std::size_t position() const { return pos_; }
  std::size_t size() const { return size_; }
  std::size_t remaining() const { return size_ - pos_; }
  bool empty() const { return pos_ >= size_; }
  const std::uint8_t* data() const { return data_; }

  // Reposition the cursor to an absolute offset. Used when following the
  // section directory, which stores absolute file offsets.
  Status seek(std::size_t absolute) {
    if (absolute > size_) return fail(Code::Truncated, "seek past end");
    pos_ = absolute;
    return Status::ok();
  }

  // Produce a sub-reader covering [offset, offset+length). The sub-reader shares
  // the parent buffer but cannot read outside its window, so a corrupt section
  // length cannot leak into neighbouring sections.
  Result<ByteReader> window(std::size_t offset, std::size_t length) const {
    if (offset > size_ || length > size_ - offset)
      return Result<ByteReader>(fail(Code::Truncated, "section window out of range"));
    return Result<ByteReader>(ByteReader(data_ + offset, length));
  }

  Status read_u8(std::uint8_t* out) {
    if (remaining() < 1) return fail(Code::Truncated, "u8");
    *out = data_[pos_++];
    return Status::ok();
  }

  Status read_u16(std::uint16_t* out) {
    if (remaining() < 2) return fail(Code::Truncated, "u16");
    std::uint16_t v = static_cast<std::uint16_t>(data_[pos_]) |
                      (static_cast<std::uint16_t>(data_[pos_ + 1]) << 8);
    pos_ += 2;
    *out = v;
    return Status::ok();
  }

  Status read_u32(std::uint32_t* out) {
    if (remaining() < 4) return fail(Code::Truncated, "u32");
    std::uint32_t v = static_cast<std::uint32_t>(data_[pos_]) |
                      (static_cast<std::uint32_t>(data_[pos_ + 1]) << 8) |
                      (static_cast<std::uint32_t>(data_[pos_ + 2]) << 16) |
                      (static_cast<std::uint32_t>(data_[pos_ + 3]) << 24);
    pos_ += 4;
    *out = v;
    return Status::ok();
  }

  Status read_u64(std::uint64_t* out) {
    if (remaining() < 8) return fail(Code::Truncated, "u64");
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
      v |= static_cast<std::uint64_t>(data_[pos_ + i]) << (8 * i);
    pos_ += 8;
    *out = v;
    return Status::ok();
  }

  // IEEE-754 double, stored as a little-endian u64 bit pattern.
  Status read_f64(double* out) {
    std::uint64_t bits = 0;
    STRATAVM_RETURN_IF_ERROR(read_u64(&bits));
    double d;
    static_assert(sizeof(d) == sizeof(bits), "double must be 8 bytes");
    std::memcpy(&d, &bits, sizeof(d));
    *out = d;
    return Status::ok();
  }

  // Unsigned LEB128. Caps the encoding at 10 bytes (enough for 64 bits) so a
  // pathological run of 0x80 continuation bytes cannot spin forever.
  Status read_varint(std::uint64_t* out) {
    std::uint64_t value = 0;
    int shift = 0;
    for (int i = 0; i < 10; ++i) {
      if (remaining() < 1) return fail(Code::Truncated, "varint");
      std::uint8_t byte = data_[pos_++];
      value |= static_cast<std::uint64_t>(byte & 0x7Fu) << shift;
      if ((byte & 0x80u) == 0) {
        *out = value;
        return Status::ok();
      }
      shift += 7;
    }
    return fail(Code::BadValue, "varint too long");
  }

  // Convenience: read a varint and reject values that do not fit a u32. Almost
  // every count/index in the format is logically 32-bit.
  Status read_varint_u32(std::uint32_t* out) {
    std::uint64_t v = 0;
    STRATAVM_RETURN_IF_ERROR(read_varint(&v));
    if (v > 0xFFFFFFFFull) return fail(Code::BadValue, "varint exceeds u32");
    *out = static_cast<std::uint32_t>(v);
    return Status::ok();
  }

  // Zig-zag signed varint (protobuf-style). Maps small magnitudes either side of
  // zero to small unsigned encodings.
  Status read_svarint(std::int64_t* out) {
    std::uint64_t u = 0;
    STRATAVM_RETURN_IF_ERROR(read_varint(&u));
    *out = static_cast<std::int64_t>((u >> 1) ^ (~(u & 1) + 1));
    return Status::ok();
  }

  // Copy `length` raw bytes out into `out`. Bounds checked.
  Status read_bytes(std::size_t length, std::string* out) {
    if (remaining() < length) return fail(Code::Truncated, "bytes");
    out->assign(reinterpret_cast<const char*>(data_ + pos_), length);
    pos_ += length;
    return Status::ok();
  }

  // Read a varint length prefix followed by that many bytes.
  Status read_lp_string(std::string* out, std::size_t max_len) {
    std::uint64_t len = 0;
    STRATAVM_RETURN_IF_ERROR(read_varint(&len));
    if (len > max_len) return fail(Code::LimitExceeded, "string too long");
    return read_bytes(static_cast<std::size_t>(len), out);
  }

  // Advance without copying. Used to skip sections we do not model.
  Status skip(std::size_t length) {
    if (remaining() < length) return fail(Code::Truncated, "skip");
    pos_ += length;
    return Status::ok();
  }

 private:
  const std::uint8_t* data_;
  std::size_t size_;
  std::size_t pos_;
};

}  // namespace stratavm

#endif  // STRATAVM_BYTE_READER_HPP
