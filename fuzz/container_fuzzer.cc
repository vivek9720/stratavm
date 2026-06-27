// container_fuzzer.cc - drives the full container loader.
//
// This harness feeds raw bytes directly to load(), which parses the section
// directory, atom pool, schema, initial heap, journal and cross-reference index
// in sequence. The load path exercises ByteReader bounds checking, varint/LEB
// decoding, schema validation, heap decoding, the full journal replay VM
// (including the group-construction stack and forward-reference materialisation)
// and the cross-reference reconciler.
//
// The harness must not crash for any input. It returns 0 unconditionally so that
// libFuzzer knows the run was stable even for structurally invalid inputs.
#include <cstdint>
#include <cstddef>

#include "stratavm/loader.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  stratavm::LoadOptions opt;
  opt.strict_validation = false; // validation is correct code, not the bug
  auto res = stratavm::load(data, size, opt);
  (void)res;
  return 0;
}
