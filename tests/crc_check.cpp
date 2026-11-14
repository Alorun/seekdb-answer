// Differential check: hardware CRC32C path equals the portable table path.
// Build: g++ -O2 -std=c++20 -Isrc tests/crc_check.cpp src/common.cpp -o /tmp/crc_check
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>
#include "common.h"

namespace mvdb {
uint32_t crc32cSw(const void*, size_t);
#if defined(__x86_64__) || defined(__i386__)
uint32_t crc32cHw(const void*, size_t);
#endif
}

int main() {
#if defined(__x86_64__) || defined(__i386__)
  if (!__builtin_cpu_supports("sse4.2")) {
    std::printf("SKIP: sse4.2 not available\n");
    return 0;
  }
#endif
  std::mt19937_64 rng(20261114);
  long checked = 0, mismatches = 0;
  for (size_t n = 0; n <= 8192; ++n) {
    std::vector<unsigned char> buf(n);
    for (auto& b : buf) b = static_cast<unsigned char>(rng());
    uint32_t sw = mvdb::crc32cSw(buf.data(), n);
    uint32_t api = mvdb::crc32c(buf.data(), n);
#if defined(__x86_64__) || defined(__i386__)
    uint32_t hw = mvdb::crc32cHw(buf.data(), n);
    if (hw != sw || api != sw) ++mismatches;
#else
    if (api != sw) ++mismatches;
#endif
    ++checked;
  }
  for (int rep = 0; rep < 50; ++rep) {
    size_t n = 1 + (rng() % 200000);
    std::vector<unsigned char> buf(n);
    for (auto& b : buf) b = static_cast<unsigned char>(rng());
    uint32_t sw = mvdb::crc32cSw(buf.data(), n);
    uint32_t api = mvdb::crc32c(buf.data(), n);
    if (api != sw) ++mismatches;
    ++checked;
  }
  std::printf("crc32c differential: checked=%ld mismatches=%ld\n", checked, mismatches);
  return mismatches ? 1 : 0;
}
