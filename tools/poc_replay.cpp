// poc_replay.cpp - standalone PoC validator: reads a file and drives the
// container loader, expecting an ASan crash on the UAF test case.
//
// Usage: poc_replay <file.poc>
//
// Compile with -fsanitize=address,undefined; run once per invocation.
// The binary exits 0 on clean parse, non-zero on detected error, and crashes
// (via ASan abort) on the UAF path.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

#include "stratavm/loader.hpp"

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: poc_replay <file.poc>\n");
    return 2;
  }
  std::ifstream f(argv[1], std::ios::binary);
  if (!f) { std::fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
  std::fprintf(stderr, "loaded %zu bytes from %s\n", bytes.size(), argv[1]);

  stratavm::LoadOptions opt;
  opt.strict_validation = false;
  auto res = stratavm::load(bytes.data(), bytes.size(), opt);
  if (res.is_error()) {
    std::fprintf(stderr, "load returned error: %s\n",
                 res.status().to_string().c_str());
  } else {
    std::fprintf(stderr, "load OK (nodes=%zu)\n",
                 res.value()->vm.stats().nodes_created);
  }
  return 0;
}
