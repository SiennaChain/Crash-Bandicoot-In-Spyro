#include "log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace csp::log {
namespace {

std::mutex g_mutex;
FILE* g_file = nullptr;

void Write(const char* level, const char* fmt, va_list args) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_file) return;
  SYSTEMTIME t;
  GetLocalTime(&t);
  std::fprintf(g_file, "%02u:%02u:%02u.%03u [%s] ", t.wHour, t.wMinute, t.wSecond,
               t.wMilliseconds, level);
  std::vfprintf(g_file, fmt, args);
  std::fputc('\n', g_file);
  std::fflush(g_file);
}

}  // namespace

void Init(const std::wstring& path) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_file) return;
  g_file = _wfopen(path.c_str(), L"w");
}

void Info(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  Write("info", fmt, args);
  va_end(args);
}

void Error(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  Write("error", fmt, args);
  va_end(args);
}

}  // namespace csp::log
