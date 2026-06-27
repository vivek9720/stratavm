// wire.hpp - streaming wire framing protocol for Stratavm containers.
//
// Wraps serialized Stratavm containers for network or IPC transport.  Each
// "frame" carries a fixed 16-byte header, a variable-length payload, and a
// trailing CRC-32 checksum so the receiver can detect corruption in transit.
//
// Frame layout (all integer fields little-endian):
//
//   [0..3]   magic       : 'S','W','F','R'  (0x52465753)
//   [4..7]   flags       : u32; bit 0 = compressed (reserved), bit 1 = EOS
//   [8..11]  seq         : u32; monotonically increasing per encoder session
//   [12..15] payload_len : u32
//   [16..16+payload_len-1] payload
//   [16+payload_len .. 16+payload_len+3] CRC-32 of bytes [0..16+payload_len)
//
// Total frame size = 16 + payload_len + 4 bytes.
// Maximum payload: 256 MiB.

#ifndef STRATAVM_WIRE_HPP
#define STRATAVM_WIRE_HPP

#include <cstddef>
#include <cstdint>
#include <vector>

#include "stratavm/status.hpp"

namespace stratavm {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

/// Four-byte magic that starts every wire frame: 'S','W','F','R'.
constexpr uint32_t kWireFrameMagic = 0x52465753u;

/// Upper bound on the payload of a single frame (256 MiB).
constexpr uint32_t kWireMaxPayload = 256u * 1024u * 1024u;

/// Size of the fixed frame header in bytes.
constexpr size_t kWireHeaderSize = 16;

/// Size of the trailing CRC-32 field in bytes.
constexpr size_t kWireCrcSize = 4;

/// Minimum frame size (header + empty payload + CRC).
constexpr size_t kWireMinFrameSize = kWireHeaderSize + kWireCrcSize;

// ---------------------------------------------------------------------------
// WireFlag
// ---------------------------------------------------------------------------

/// Flag bits stored in the frame's flags field.
enum class WireFlag : uint32_t {
  /// Payload is compressed (reserved – not yet implemented).
  Compressed  = 1u << 0,
  /// This is the final frame in the session (payload will be empty).
  EndOfStream = 1u << 1,
};

// ---------------------------------------------------------------------------
// WireFrame
// ---------------------------------------------------------------------------

/// A decoded wire frame.  The payload bytes are owned by this struct.
struct WireFrame {
  uint32_t             seq;      ///< Sequence number from the frame header.
  uint32_t             flags;    ///< Raw flags word.
  std::vector<uint8_t> payload;  ///< Frame payload (empty for EOS frames).

  /// Returns true when the EndOfStream flag is set.
  bool is_eos() const noexcept {
    return (flags & static_cast<uint32_t>(WireFlag::EndOfStream)) != 0u;
  }
};

// ---------------------------------------------------------------------------
// CRC-32 helper
// ---------------------------------------------------------------------------

/// Standard CRC-32/ISO-HDLC (polynomial 0xEDB88320) over the given byte span.
/// The table is generated on the first call and cached for subsequent calls.
uint32_t wire_crc32(const uint8_t* data, size_t len);

// ---------------------------------------------------------------------------
// WireEncoder
// ---------------------------------------------------------------------------

/// Serialises frames into a flat byte buffer that the caller drains and sends
/// over a socket / pipe.
///
/// Usage:
///   WireEncoder enc;
///   enc.encode_container(my_bytes);  // one or more times
///   enc.encode_eos();
///   auto out = enc.take();           // bytes ready to write()
class WireEncoder {
 public:
  WireEncoder() = default;

  /// Encode an arbitrary payload with optional extra flags.
  void encode_frame(const uint8_t* payload, size_t len, uint32_t flags = 0);

  /// Encode a serialised Stratavm container as a single frame.
  void encode_container(const std::vector<uint8_t>& container_bytes);

  /// Append an end-of-stream frame (empty payload, EndOfStream flag set).
  void encode_eos();

  /// Move all buffered output bytes out of the encoder.  Resets the buffer.
  std::vector<uint8_t> take();

  /// How many bytes are waiting to be drained.
  size_t buffered_bytes() const noexcept { return buf_.size(); }

 private:
  uint32_t             next_seq_ = 0;
  std::vector<uint8_t> buf_;
};

// ---------------------------------------------------------------------------
// WireDecoder
// ---------------------------------------------------------------------------

/// Reassembles frames from an incoming byte stream.
///
/// The decoder is purely streaming: the caller feeds raw bytes in whatever
/// chunk sizes arrive from the socket, then polls next_frame() to pull out
/// complete decoded frames.
///
/// Usage:
///   WireDecoder dec;
///   dec.feed(incoming_bytes);
///   while (dec.has_frame()) {
///     auto res = dec.next_frame();
///     if (!res) handle_error(res.status());
///     else      process(res.value());
///   }
class WireDecoder {
 public:
  WireDecoder() = default;

  /// Push raw bytes into the internal reassembly buffer.
  void feed(const uint8_t* data, size_t n);

  /// Overload that accepts a vector directly.
  void feed(const std::vector<uint8_t>& data);

  /// Attempt to parse and return the next complete frame.
  /// Returns Code::Truncated when more bytes are needed.
  /// Returns Code::BadMagic / Code::BadChecksum on corrupt data.
  Result<WireFrame> next_frame();

  /// Returns true if there are enough buffered bytes to parse at least one
  /// complete frame (i.e., next_frame() will not return Truncated).
  bool has_frame() const;

  /// Discard all buffered bytes and reset the position cursor.
  void reset();

  /// Number of buffered bytes not yet consumed by next_frame().
  size_t pending_bytes() const noexcept;

 private:
  /// Compact the buffer when pos_ has grown past half its capacity,
  /// keeping only the unconsumed tail.
  void maybe_compact();

  std::vector<uint8_t> buf_;
  size_t               pos_ = 0;
};

// ---------------------------------------------------------------------------
// WireSession
// ---------------------------------------------------------------------------

/// A paired encoder + decoder representing one logical connection endpoint.
///
/// Callers write containers via send_container(), flush bytes to the network
/// with flush_tx(), and feed received bytes with receive_bytes().
class WireSession {
 public:
  WireSession() = default;

  // ---- Sending side -------------------------------------------------------

  /// Encode a container into the session's transmit buffer.
  void send_container(const std::vector<uint8_t>& bytes);

  /// Encode an end-of-stream sentinel into the transmit buffer.
  void send_eos();

  /// Drain and return all bytes accumulated in the transmit buffer.
  /// The caller is responsible for writing these bytes to the socket/pipe.
  std::vector<uint8_t> flush_tx();

  // ---- Receiving side -----------------------------------------------------

  /// Feed raw bytes arriving from the peer into the receive decoder.
  void receive_bytes(const uint8_t* data, size_t n);

  /// Pull the next fully reassembled frame from the receive buffer.
  /// Returns Code::Truncated when no complete frame is available yet.
  Result<WireFrame> next_received_frame();

 private:
  WireEncoder encoder_;
  WireDecoder decoder_;
};

}  // namespace stratavm

#endif  // STRATAVM_WIRE_HPP
