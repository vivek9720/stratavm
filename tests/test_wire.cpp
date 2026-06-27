// test_wire.cpp - unit tests for the Stratavm wire framing protocol.
//
// Build alongside the library objects:
//   clang++ -std=c++17 -Iinclude -Itests \
//       tests/test_wire.cpp src/wire.cpp src/status.cpp \
//       -o build/test_wire
//
// Or just add src/wire.cpp to LIB_SRCS in the Makefile and run `make tests`.

#include "test_runner.hpp"
#include "stratavm/wire.hpp"

#include <cstring>
#include <numeric>
#include <vector>

using namespace stratavm;
using namespace stratavm::test;
using stratavm::Code;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Build a simple payload vector of `n` bytes with value 0x00..0xFF cycling.
static std::vector<uint8_t> make_payload(size_t n, uint8_t start = 0) {
  std::vector<uint8_t> v(n);
  for (size_t i = 0; i < n; ++i)
    v[i] = static_cast<uint8_t>((start + i) & 0xFFu);
  return v;
}

// ---------------------------------------------------------------------------
// Test 1: encode one container, decode it, verify payload matches
// ---------------------------------------------------------------------------
STEST(wire_encode_decode_single) {
  const auto data = make_payload(32, 0xAB);

  WireEncoder enc;
  enc.encode_container(data);
  auto wire_bytes = enc.take();

  // wire_bytes should be non-empty and larger than the raw payload.
  REQUIRE(wire_bytes.size() == kWireHeaderSize + data.size() + kWireCrcSize);

  WireDecoder dec;
  dec.feed(wire_bytes);
  REQUIRE(dec.has_frame());

  auto res = dec.next_frame();
  REQUIRE_OK(res.status());

  const WireFrame& f = res.value();
  REQUIRE_EQ(f.seq, 0u);
  REQUIRE_EQ(f.flags, 0u);
  REQUIRE(!f.is_eos());
  REQUIRE_EQ(f.payload.size(), data.size());
  REQUIRE(f.payload == data);

  // No more frames.
  auto res2 = dec.next_frame();
  REQUIRE(res2.is_error());
  REQUIRE_EQ(res2.status().code(), Code::Truncated);
}

// ---------------------------------------------------------------------------
// Test 2: encode 3 containers, decode all 3 in order
// ---------------------------------------------------------------------------
STEST(wire_encode_decode_multi) {
  const auto d0 = make_payload(8,  0x00);
  const auto d1 = make_payload(16, 0x10);
  const auto d2 = make_payload(4,  0x20);

  WireEncoder enc;
  enc.encode_container(d0);
  enc.encode_container(d1);
  enc.encode_container(d2);
  auto wire_bytes = enc.take();

  WireDecoder dec;
  dec.feed(wire_bytes);

  {
    auto r = dec.next_frame();
    REQUIRE_OK(r.status());
    REQUIRE_EQ(r.value().seq, 0u);
    REQUIRE(r.value().payload == d0);
  }
  {
    auto r = dec.next_frame();
    REQUIRE_OK(r.status());
    REQUIRE_EQ(r.value().seq, 1u);
    REQUIRE(r.value().payload == d1);
  }
  {
    auto r = dec.next_frame();
    REQUIRE_OK(r.status());
    REQUIRE_EQ(r.value().seq, 2u);
    REQUIRE(r.value().payload == d2);
  }
  // Stream exhausted.
  REQUIRE(dec.next_frame().status().code() == Code::Truncated);
}

// ---------------------------------------------------------------------------
// Test 3: EOS frame detected
// ---------------------------------------------------------------------------
STEST(wire_eos_detected) {
  const auto data = make_payload(10, 0x55);

  WireEncoder enc;
  enc.encode_container(data);
  enc.encode_eos();
  auto wire_bytes = enc.take();

  WireDecoder dec;
  dec.feed(wire_bytes);

  // First frame: container
  {
    auto r = dec.next_frame();
    REQUIRE_OK(r.status());
    REQUIRE(!r.value().is_eos());
    REQUIRE(r.value().payload == data);
  }

  // Second frame: EOS
  {
    auto r = dec.next_frame();
    REQUIRE_OK(r.status());
    REQUIRE(r.value().is_eos());
    REQUIRE(r.value().payload.empty());
  }
}

// ---------------------------------------------------------------------------
// Test 4: CRC corruption is detected
// ---------------------------------------------------------------------------
STEST(wire_crc_mismatch_detected) {
  const auto data = make_payload(20, 0xCC);

  WireEncoder enc;
  enc.encode_container(data);
  auto wire_bytes = enc.take();

  // Flip a byte inside the payload (byte at index 20, which is within the
  // payload region starting at offset kWireHeaderSize = 16).
  REQUIRE(wire_bytes.size() > 20);
  wire_bytes[20] ^= 0xFFu;

  WireDecoder dec;
  dec.feed(wire_bytes);

  auto r = dec.next_frame();
  REQUIRE(r.is_error());
  REQUIRE_EQ(r.status().code(), Code::BadChecksum);
}

// ---------------------------------------------------------------------------
// Test 5: magic corruption is detected
// ---------------------------------------------------------------------------
STEST(wire_magic_mismatch) {
  const auto data = make_payload(8, 0x11);

  WireEncoder enc;
  enc.encode_container(data);
  auto wire_bytes = enc.take();

  // Corrupt the very first byte of the magic word.
  wire_bytes[0] ^= 0x01u;

  WireDecoder dec;
  dec.feed(wire_bytes);

  auto r = dec.next_frame();
  REQUIRE(r.is_error());
  REQUIRE_EQ(r.status().code(), Code::BadMagic);
}

// ---------------------------------------------------------------------------
// Test 6: feeding only half a frame returns Truncated
// ---------------------------------------------------------------------------
STEST(wire_truncated_returns_error) {
  const auto data = make_payload(64, 0x77);

  WireEncoder enc;
  enc.encode_container(data);
  auto wire_bytes = enc.take();

  // Only feed the first half.
  const size_t half = wire_bytes.size() / 2;

  WireDecoder dec;
  dec.feed(wire_bytes.data(), half);

  auto r = dec.next_frame();
  REQUIRE(r.is_error());
  REQUIRE_EQ(r.status().code(), Code::Truncated);
}

// ---------------------------------------------------------------------------
// Test 7: byte-at-a-time feeding still assembles the frame correctly
// ---------------------------------------------------------------------------
STEST(wire_partial_feed) {
  const auto data = make_payload(50, 0x42);

  WireEncoder enc;
  enc.encode_container(data);
  auto wire_bytes = enc.take();

  WireDecoder dec;

  // Feed one byte at a time until has_frame() becomes true.
  size_t fed = 0;
  while (!dec.has_frame()) {
    REQUIRE(fed < wire_bytes.size());
    dec.feed(&wire_bytes[fed], 1);
    ++fed;
  }

  auto r = dec.next_frame();
  REQUIRE_OK(r.status());
  REQUIRE(r.value().payload == data);
}

// ---------------------------------------------------------------------------
// Test 8: sequence numbers increment monotonically
// ---------------------------------------------------------------------------
STEST(wire_seq_increments) {
  WireEncoder enc;
  const int N = 5;
  for (int i = 0; i < N; ++i)
    enc.encode_container(make_payload(4, static_cast<uint8_t>(i)));
  auto wire_bytes = enc.take();

  WireDecoder dec;
  dec.feed(wire_bytes);

  for (uint32_t expected_seq = 0; expected_seq < static_cast<uint32_t>(N); ++expected_seq) {
    auto r = dec.next_frame();
    REQUIRE_OK(r.status());
    REQUIRE_EQ(r.value().seq, expected_seq);
  }
}

// ---------------------------------------------------------------------------
// Test 9: large payload (64 KiB) round-trips correctly
// ---------------------------------------------------------------------------
STEST(wire_large_payload) {
  constexpr size_t kSize = 64u * 1024u;
  auto data = make_payload(kSize, 0x00);

  WireEncoder enc;
  enc.encode_container(data);
  auto wire_bytes = enc.take();

  REQUIRE_EQ(wire_bytes.size(), kWireHeaderSize + kSize + kWireCrcSize);

  WireDecoder dec;
  dec.feed(wire_bytes);

  auto r = dec.next_frame();
  REQUIRE_OK(r.status());
  REQUIRE_EQ(r.value().payload.size(), kSize);
  REQUIRE(r.value().payload == data);
}

// ---------------------------------------------------------------------------
// Test 10: WireSession round-trip
// ---------------------------------------------------------------------------
STEST(wire_session_roundtrip) {
  const auto d0 = make_payload(12, 0xA0);
  const auto d1 = make_payload(24, 0xB0);

  // Sender side.
  WireSession sender;
  sender.send_container(d0);
  sender.send_container(d1);
  sender.send_eos();
  auto tx_bytes = sender.flush_tx();

  // Receiver side: simulate receiving all bytes in one chunk.
  WireSession receiver;
  receiver.receive_bytes(tx_bytes.data(), tx_bytes.size());

  {
    auto r = receiver.next_received_frame();
    REQUIRE_OK(r.status());
    REQUIRE(!r.value().is_eos());
    REQUIRE_EQ(r.value().seq, 0u);
    REQUIRE(r.value().payload == d0);
  }
  {
    auto r = receiver.next_received_frame();
    REQUIRE_OK(r.status());
    REQUIRE(!r.value().is_eos());
    REQUIRE_EQ(r.value().seq, 1u);
    REQUIRE(r.value().payload == d1);
  }
  {
    auto r = receiver.next_received_frame();
    REQUIRE_OK(r.status());
    REQUIRE(r.value().is_eos());
    REQUIRE_EQ(r.value().seq, 2u);
    REQUIRE(r.value().payload.empty());
  }
  // Nothing more.
  REQUIRE_EQ(receiver.next_received_frame().status().code(), Code::Truncated);
}

// ---------------------------------------------------------------------------
// Bonus test: encoder buffer drains correctly after take()
// ---------------------------------------------------------------------------
STEST(wire_encoder_drains_on_take) {
  WireEncoder enc;
  enc.encode_container(make_payload(8, 0x01));
  REQUIRE(enc.buffered_bytes() > 0);
  auto bytes = enc.take();
  REQUIRE(bytes.size() > 0);
  // After take() the encoder buffer should be empty.
  REQUIRE_EQ(enc.buffered_bytes(), 0u);
}

// ---------------------------------------------------------------------------
// Bonus test: decoder reset clears state
// ---------------------------------------------------------------------------
STEST(wire_decoder_reset) {
  WireDecoder dec;
  dec.feed(make_payload(10, 0x00));
  REQUIRE(dec.pending_bytes() == 10u);
  dec.reset();
  REQUIRE_EQ(dec.pending_bytes(), 0u);

  // After reset the decoder should behave as if freshly constructed.
  auto r = dec.next_frame();
  REQUIRE_EQ(r.status().code(), Code::Truncated);
}

// ---------------------------------------------------------------------------
// Bonus test: wire_crc32 consistency check
// ---------------------------------------------------------------------------
STEST(wire_crc32_known_value) {
  // CRC-32 of the ASCII string "123456789" is the well-known value 0xCBF43926.
  const uint8_t msg[] = {'1','2','3','4','5','6','7','8','9'};
  const uint32_t crc = wire_crc32(msg, sizeof(msg));
  REQUIRE_EQ(crc, 0xCBF43926u);
}

// ---------------------------------------------------------------------------
// Bonus test: empty payload container is valid
// ---------------------------------------------------------------------------
STEST(wire_empty_payload_container) {
  WireEncoder enc;
  std::vector<uint8_t> empty;
  enc.encode_container(empty);
  auto wire_bytes = enc.take();

  // An empty-payload frame should still have the header + CRC.
  REQUIRE_EQ(wire_bytes.size(), kWireHeaderSize + kWireCrcSize);

  WireDecoder dec;
  dec.feed(wire_bytes);

  auto r = dec.next_frame();
  REQUIRE_OK(r.status());
  REQUIRE(r.value().payload.empty());
  REQUIRE(!r.value().is_eos());
}

// ---------------------------------------------------------------------------
// Bonus test: feed vector overload works identically to pointer overload
// ---------------------------------------------------------------------------
STEST(wire_feed_vector_overload) {
  const auto data = make_payload(16, 0xDE);

  WireEncoder enc;
  enc.encode_container(data);
  auto wire_bytes = enc.take();

  WireDecoder dec;
  dec.feed(wire_bytes);  // vector overload

  auto r = dec.next_frame();
  REQUIRE_OK(r.status());
  REQUIRE(r.value().payload == data);
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
  return stratavm::test::run_all(argc, argv);
}
