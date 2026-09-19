// Congestion Recovery - minimal test harness. No timeouts anywhere.
// Copyright 2026 Summon Software Labs.
//
// Debug builds additionally enable the MSVC debug-heap validator, which checks
// every freed block's integrity. AddressSanitizer is not installed in every
// toolchain, so heap validation is the portable equivalent used here.
#pragma once

#include <cstdio>
#include <string>

#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

namespace crtest {

inline int g_failures = 0;
inline int g_checks = 0;
inline const char* g_section = "init";

#if defined(_MSC_VER) && defined(_DEBUG)
inline _CrtMemState g_checkpoint{};
inline bool g_heap_checked = false;
#endif

inline void init() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
#if defined(_MSC_VER) && defined(_DEBUG)
  _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_CHECK_DEFAULT_DF | _CRTDBG_LEAK_CHECK_DF);
  _CrtMemCheckpoint(&g_checkpoint);
  g_heap_checked = true;
#endif
}

inline void section(const char* name) {
  g_section = name;
  std::printf("[section] %s\n", name);
}

inline void check(bool condition, const char* expression, const char* file, int line) {
  ++g_checks;
  if (!condition) {
    ++g_failures;
    std::printf("FAIL %s:%d [%s] %s\n", file, line, g_section, expression);
  }
}

inline int summary(const char* suite) {
#if defined(_MSC_VER) && defined(_DEBUG)
  if (g_heap_checked) {
    _CrtMemState now{};
    _CrtMemState difference{};
    _CrtMemCheckpoint(&now);
    if (_CrtMemDifference(&difference, &g_checkpoint, &now) != 0) {
      ++g_failures;
      std::printf("FAIL debug heap: %lld blocks and %lld bytes were not returned\n",
                  static_cast<long long>(difference.lCounts[_NORMAL_BLOCK]),
                  static_cast<long long>(difference.lSizes[_NORMAL_BLOCK]));
    }
  }
#endif
  std::printf("%s: %d checks, %d failures\n", suite, g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}

}  // namespace crtest

#define CHK(cond) ::crtest::check((cond), #cond, __FILE__, __LINE__)
#define SECTION(name) ::crtest::section(name)
