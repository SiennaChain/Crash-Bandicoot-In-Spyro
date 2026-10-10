// The one clock both bridges agree on.
#pragma once

#include <windows.h>

#include <cstdint>

namespace csp {

// Microseconds on the system performance counter, low 32 bits. The counter is
// system-wide, so a value taken in one game means the same instant in the
// other. Compare two with ElapsedUs (csp_protocol.h); it wraps.
inline uint32_t NowUs() {
  static const LONGLONG frequency = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return f.QuadPart;
  }();
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  // Split to avoid overflowing 64 bits on a long-running machine.
  const LONGLONG seconds = now.QuadPart / frequency;
  const LONGLONG remainder = now.QuadPart % frequency;
  return static_cast<uint32_t>(seconds * 1000000 + remainder * 1000000 / frequency);
}

}  // namespace csp
