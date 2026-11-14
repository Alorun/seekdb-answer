// Differential check: parseVectorLiteral's fast decimal path must produce the
// same float32 bits as the original from_chars(double) -> float reference.
// Build: g++ -O2 -std=c++20 -Isrc tests/parse_vector_check.cpp src/common.cpp -o /tmp/parse_check
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>
#include "common.h"

static bool reference(const std::string& text, std::vector<float>& out) {
  out.clear();
  std::string s = text;
  if (s.size() < 2 || s.front() != '[' || s.back() != ']') return false;
  std::string body = s.substr(1, s.size() - 2);
  size_t pos = 0;
  while (pos < body.size()) {
    size_t comma = body.find(',', pos);
    std::string tok = body.substr(pos, comma == std::string::npos ? comma : comma - pos);
    double d = 0;
    auto r = std::from_chars(tok.data(), tok.data() + tok.size(), d);
    if (r.ec != std::errc() || r.ptr != tok.data() + tok.size()) return false;
    if (!std::isfinite(d) || std::fabs(d) > 3.4028234663852886e38) return false;
    float f = static_cast<float>(d);
    if (!std::isfinite(f)) return false;
    out.push_back(f);
    if (comma == std::string::npos) break;
    pos = comma + 1;
  }
  return true;
}

int main() {
  std::mt19937_64 rng(20261114);
  long checked = 0, mismatches = 0, rejected = 0;
  for (int rep = 0; rep < 20000; ++rep) {
    int n = 1 + static_cast<int>(rng() % 16);
    std::string body;
    std::vector<float> want;
    for (int j = 0; j < n; ++j) {
      char buf[64];
      int mode = static_cast<int>(rng() % 4);
      int64_t ip = static_cast<int64_t>(rng() % 1000000);
      int frac = static_cast<int>(rng() % 7);
      int fexp = static_cast<int>(rng() % 9);
      int exp10 = static_cast<int>(rng() % 61) - 30;
      if (mode == 0) std::snprintf(buf, sizeof(buf), "%s%lld.%0*d",
                                   (rng() & 1) ? "-" : "", (long long)ip, frac, fexp);
      else if (mode == 1) std::snprintf(buf, sizeof(buf), "%s%llde%d",
                                        (rng() & 1) ? "-" : "", (long long)ip, exp10);
      else if (mode == 2) std::snprintf(buf, sizeof(buf), "%s%lld.%0*de%d",
                                        (rng() & 1) ? "-" : "", (long long)ip, frac, fexp, exp10);
      else std::snprintf(buf, sizeof(buf), "%s%lld", (rng() & 1) ? "-" : "",
                         (long long)ip);
      if (j) body.push_back(',');
      body += buf;
    }
    std::string text = "[" + body + "]";
    std::vector<float> got;
    std::string err;
    bool ok = mvdb::parseVectorLiteral(text, got, err);
    bool ref = reference(text, want);
    if (!ref) { ++rejected; continue; }
    if (!ok || got.size() != want.size() ||
        (want.size() && std::memcmp(got.data(), want.data(),
                                    want.size() * sizeof(float)) != 0)) {
      if (mismatches < 5) std::printf("mismatch: %s\n", text.c_str());
      ++mismatches;
    }
    ++checked;
  }
  std::printf("parseVectorLiteral differential: checked=%ld rejected=%ld mismatches=%ld\n",
              checked, rejected, mismatches);
  return mismatches ? 1 : 0;
}
