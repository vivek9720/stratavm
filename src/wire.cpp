// wire.cpp - implementation of the Stratavm wire framing protocol.
//
// See include/stratavm/wire.hpp for the frame layout and API documentation.
//
// Notes:
//   * No external dependencies – CRC-32 is computed via an inline lookup table.
//   * All multi-byte integers are stored in little-endian byte order, matching
//     the rest of the Stratavm on-disk format.
//   * The decoder maintains a single contiguous byte buffer and a logical read
//     cursor (pos_).  When pos_ has advanced past half the buffer the leading
//     consumed bytes are erased so the allocation does not grow without bound
//     over long-lived connections.

#include "stratavm/wire.hpp"

#include <array>
#include <cassert>
#include <cstring>
#include <stdexcept>

namespace stratavm {

// ===========================================================================
// CRC-32 (ISO-HDLC, polynomial 0xEDB88320)
// ===========================================================================

namespace {

/// Build and return a reference to the 256-entry CRC lookup table.
/// The table is computed once on first access and then reused.
const std::array<uint32_t, 256>& crc_table() {
  static const std::array<uint32_t, 256> tbl = []() {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) {
        if (c & 1u)
          c = 0xEDB88320u ^ (c >> 1);
        else
          c >>= 1;
      }
      t[i] = c;
    }
    return t;
  }();
  return tbl;
}

/// Write a 32-bit value to `dst` in little-endian byte order.
inline void write_le32(uint8_t* dst, uint32_t v) noexcept {
  dst[0] = static_cast<uint8_t>(v & 0xFFu);
  dst[1] = static_cast<uint8_t>((v >>  8) & 0xFFu);
  dst[2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
  dst[3] = static_cast<uint8_t>((v >> 24) & 0xFFu);
}

/// Read a 32-bit little-endian value from `src`.
inline uint32_t read_le32(const uint8_t* src) noexcept {
  return  static_cast<uint32_t>(src[0])        |
         (static_cast<uint32_t>(src[1]) <<  8) |
         (static_cast<uint32_t>(src[2]) << 16) |
         (static_cast<uint32_t>(src[3]) << 24);
}

}  // namespace

// ---------------------------------------------------------------------------
// wire_crc32
// ---------------------------------------------------------------------------

uint32_t wire_crc32(const uint8_t* data, size_t len) {
  const auto& tbl = crc_table();
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; ++i) {
    crc = tbl[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFFu;
}

// ===========================================================================
// WireEncoder
// ===========================================================================

void WireEncoder::encode_frame(const uint8_t* payload, size_t len,
                                uint32_t flags) {
  if (len > kWireMaxPayload) {
    // Hard limit: silently truncate is dangerous; callers should not hit this.
    // We abort rather than silently corrupt the stream.
    throw std::length_error("WireEncoder: payload exceeds kWireMaxPayload");
  }

  // Reserve space for header + payload + CRC.
  const size_t frame_size = kWireHeaderSize + len + kWireCrcSize;
  const size_t base = buf_.size();
  buf_.resize(base + frame_size);

  uint8_t* p = buf_.data() + base;

  // --- Header (16 bytes) ---
  write_le32(p + 0,  kWireFrameMagic);
  write_le32(p + 4,  flags);
  write_le32(p + 8,  next_seq_++);
  write_le32(p + 12, static_cast<uint32_t>(len));

  // --- Payload ---
  if (len > 0) {
    std::memcpy(p + kWireHeaderSize, payload, len);
  }

  // --- CRC-32 over (header + payload) ---
  const uint32_t crc = wire_crc32(p, kWireHeaderSize + len);
  write_le32(p + kWireHeaderSize + len, crc);
}

void WireEncoder::encode_container(const std::vector<uint8_t>& container_bytes) {
  encode_frame(container_bytes.data(), container_bytes.size(), 0u);
}

void WireEncoder::encode_eos() {
  encode_frame(nullptr, 0, static_cast<uint32_t>(WireFlag::EndOfStream));
}

std::vector<uint8_t> WireEncoder::take() {
  std::vector<uint8_t> out;
  out.swap(buf_);
  return out;
}

// ===========================================================================
// WireDecoder
// ===========================================================================

void WireDecoder::feed(const uint8_t* data, size_t n) {
  buf_.insert(buf_.end(), data, data + n);
}

void WireDecoder::feed(const std::vector<uint8_t>& data) {
  feed(data.data(), data.size());
}

void WireDecoder::reset() {
  buf_.clear();
  pos_ = 0;
}

size_t WireDecoder::pending_bytes() const noexcept {
  return (pos_ <= buf_.size()) ? (buf_.size() - pos_) : 0;
}

void WireDecoder::maybe_compact() {
  // When the consumed prefix is more than half the total buffer, shift the
  // remaining bytes down so the allocation does not grow indefinitely.
  if (pos_ > 0 && pos_ >= buf_.size() / 2) {
    buf_.erase(buf_.begin(), buf_.begin() + static_cast<ptrdiff_t>(pos_));
    pos_ = 0;
  }
}

bool WireDecoder::has_frame() const {
  const size_t avail = (pos_ <= buf_.size()) ? (buf_.size() - pos_) : 0;
  if (avail < kWireMinFrameSize) return false;

  // Peek at payload_len without consuming anything.
  const uint8_t* p = buf_.data() + pos_;
  const uint32_t payload_len = read_le32(p + 12);
  const size_t total = kWireHeaderSize + static_cast<size_t>(payload_len) + kWireCrcSize;
  return avail >= total;
}

Result<WireFrame> WireDecoder::next_frame() {
  maybe_compact();

  const size_t avail = (pos_ <= buf_.size()) ? (buf_.size() - pos_) : 0;

  // Need at least the minimum frame (header + empty payload + CRC).
  if (avail < kWireMinFrameSize) {
    return Result<WireFrame>(fail(Code::Truncated, "need more bytes for frame header"));
  }

  const uint8_t* p = buf_.data() + pos_;

  // --- Magic ---
  const uint32_t magic = read_le32(p + 0);
  if (magic != kWireFrameMagic) {
    return Result<WireFrame>(fail(Code::BadMagic, "wire frame magic mismatch"));
  }

  // --- Header fields ---
  const uint32_t flags       = read_le32(p + 4);
  const uint32_t seq         = read_le32(p + 8);
  const uint32_t payload_len = read_le32(p + 12);

  // Guard against absurdly large payloads before doing any arithmetic.
  if (payload_len > kWireMaxPayload) {
    return Result<WireFrame>(fail(Code::LimitExceeded, "wire payload_len exceeds limit"));
  }

  const size_t total = kWireHeaderSize + static_cast<size_t>(payload_len) + kWireCrcSize;

  // Do we have the full frame yet?
  if (avail < total) {
    return Result<WireFrame>(fail(Code::Truncated, "incomplete frame payload"));
  }

  // --- CRC verification ---
  // CRC covers all bytes from offset 0 through the end of the payload
  // (i.e. header + payload, excluding the trailing CRC word itself).
  const size_t crc_input_len = kWireHeaderSize + static_cast<size_t>(payload_len);
  const uint32_t computed_crc = wire_crc32(p, crc_input_len);
  const uint32_t stored_crc   = read_le32(p + crc_input_len);

  if (computed_crc != stored_crc) {
    return Result<WireFrame>(fail(Code::BadChecksum, "wire frame CRC mismatch"));
  }

  // --- Build the result ---
  WireFrame frame;
  frame.seq   = seq;
  frame.flags = flags;
  if (payload_len > 0) {
    frame.payload.assign(p + kWireHeaderSize,
                         p + kWireHeaderSize + payload_len);
  }

  // Advance past the consumed frame.
  pos_ += total;

  return Result<WireFrame>(std::move(frame));
}

// ===========================================================================
// WireSession
// ===========================================================================

void WireSession::send_container(const std::vector<uint8_t>& bytes) {
  encoder_.encode_container(bytes);
}

void WireSession::send_eos() {
  encoder_.encode_eos();
}

std::vector<uint8_t> WireSession::flush_tx() {
  return encoder_.take();
}

void WireSession::receive_bytes(const uint8_t* data, size_t n) {
  decoder_.feed(data, n);
}

Result<WireFrame> WireSession::next_received_frame() {
  return decoder_.next_frame();
}

}  // namespace stratavm
