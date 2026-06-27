#include "test_runner.hpp"
#include "stratavm/byte_reader.hpp"
#include "stratavm/byte_writer.hpp"
#include <cmath>

using namespace stratavm;
using namespace stratavm::test;
using stratavm::Code;

STEST(byte_reader_u8) {
  uint8_t buf[] = {0xAB};
  ByteReader r(buf, 1);
  uint8_t v = 0;
  REQUIRE_OK(r.read_u8(&v));
  REQUIRE_EQ(v, 0xABu);
  REQUIRE(r.empty());
}

STEST(byte_reader_truncated_u32) {
  uint8_t buf[] = {0x01, 0x02};
  ByteReader r(buf, 2);
  uint32_t v = 0;
  auto st = r.read_u32(&v);
  REQUIRE_ERR(st, Code::Truncated);
}

STEST(byte_reader_u32_le) {
  uint8_t buf[] = {0x01, 0x00, 0x00, 0x00};
  ByteReader r(buf, 4);
  uint32_t v = 0;
  REQUIRE_OK(r.read_u32(&v));
  REQUIRE_EQ(v, 1u);
}

STEST(varint_roundtrip) {
  ByteWriter w;
  uint64_t vals[] = {0, 1, 127, 128, 300, 16383, 16384, 0xFFFFFFFFull, 0xFFFFFFFFFFFFFFull};
  for (auto v : vals) w.varint(v);
  auto bytes = w.take();
  ByteReader r(bytes.data(), bytes.size());
  for (auto expected : vals) {
    uint64_t got = 0;
    REQUIRE_OK(r.read_varint(&got));
    REQUIRE_EQ(got, expected);
  }
}

STEST(svarint_roundtrip) {
  ByteWriter w;
  int64_t vals[] = {0, 1, -1, 63, -64, 127, -128, 1000000, -1000000};
  for (auto v : vals) w.svarint(v);
  auto bytes = w.take();
  ByteReader r(bytes.data(), bytes.size());
  for (auto expected : vals) {
    int64_t got = 0;
    REQUIRE_OK(r.read_svarint(&got));
    REQUIRE_EQ(got, expected);
  }
}

STEST(f64_roundtrip) {
  ByteWriter w;
  double vals[] = {0.0, 1.0, -1.0, 3.14159265358979, 1e300, -1e-300};
  for (auto v : vals) w.f64(v);
  auto bytes = w.take();
  ByteReader r(bytes.data(), bytes.size());
  for (auto expected : vals) {
    double got = 0;
    REQUIRE_OK(r.read_f64(&got));
    REQUIRE(expected == got || (std::isnan(expected) && std::isnan(got)));
  }
}

STEST(lp_string_roundtrip) {
  ByteWriter w;
  std::string s = "hello, stratavm!";
  w.lp_string(s);
  auto bytes = w.take();
  ByteReader r(bytes.data(), bytes.size());
  std::string got;
  REQUIRE_OK(r.read_lp_string(&got, 4096));
  REQUIRE_EQ(got, s);
}

STEST(window_out_of_range) {
  uint8_t buf[8] = {};
  ByteReader r(buf, 8);
  auto res = r.window(4, 8);
  REQUIRE(res.is_error());
  REQUIRE_EQ(res.status().code(), Code::Truncated);
}

STEST(seek_and_reread) {
  uint8_t buf[] = {0x01, 0x02, 0x03, 0x04};
  ByteReader r(buf, 4);
  uint8_t v = 0;
  REQUIRE_OK(r.read_u8(&v)); REQUIRE_EQ(v, 1u);
  REQUIRE_OK(r.seek(0));
  REQUIRE_OK(r.read_u8(&v)); REQUIRE_EQ(v, 1u);
}

int main(int argc, char** argv) { return run_all(argc, argv); }
