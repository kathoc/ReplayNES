#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

#include "rn_test.h"

namespace rntest {
namespace {
struct Case { const char* name; Fn fn; const char* file; int line; };
std::vector<Case>& registry() { static std::vector<Case> r; return r; }
int gFailures = 0;
}  // namespace

Registrar::Registrar(const char* name, Fn fn, const char* file, int line) { registry().push_back({name, fn, file, line}); }

void reportFailure(const char* file, int line, const std::string& what) {
  ++gFailures;
  std::fprintf(stderr, "%s:%d: FAILED: %s\n", file, line, what.c_str());
}

void message(const char* file, int line, const std::string& what) {
  std::fprintf(stdout, "  [msg] %s:%d: %s\n", file, line, what.c_str());
}
}  // namespace rntest

// Usage: <test> [--list] [substring filter...]
int main(int argc, char** argv) {
  using namespace rntest;
  std::vector<std::string> filters;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--list")) {
      for (auto& c : registry()) std::printf("%s\n", c.name);
      return 0;
    }
    filters.push_back(argv[i]);
  }
  int failedCases = 0, ran = 0;
  for (auto& c : registry()) {
    if (!filters.empty()) {
      bool match = false;
      for (auto& f : filters) match = match || std::string(c.name).find(f) != std::string::npos;
      if (!match) continue;
    }
    ++ran;
    int before = gFailures;
    auto t0 = std::chrono::steady_clock::now();
    try {
      c.fn();
    } catch (const RequireFailure&) {
    } catch (const std::exception& e) {
      reportFailure(c.file, c.line, std::string("unexpected exception: ") + e.what());
    } catch (...) {
      reportFailure(c.file, c.line, "unexpected unknown exception");
    }
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    bool ok = gFailures == before;
    if (!ok) ++failedCases;
    std::printf("[%s] %s (%.0f ms)\n", ok ? " OK " : "FAIL", c.name, ms);
  }
  std::printf("%d test case(s), %d failed, %d assertion failure(s)\n", ran, failedCases, gFailures);
  return failedCases ? 1 : 0;
}
