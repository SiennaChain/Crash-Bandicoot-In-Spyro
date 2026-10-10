#include "crashlog.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>

#include "log.h"

namespace csp::crashlog {
namespace {

std::atomic<int> g_logged{0};

// "module.dll+0x1234" for an address inside a loaded module.
void Describe(const void* address, char* out, size_t size) {
  HMODULE module = nullptr;
  char path[MAX_PATH] = "";
  if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         static_cast<LPCSTR>(address), &module) &&
      GetModuleFileNameA(module, path, MAX_PATH)) {
    const char* name = std::strrchr(path, '\\');
    std::snprintf(out, size, "%s+0x%lX", name ? name + 1 : path,
                  static_cast<unsigned long>(reinterpret_cast<UINT_PTR>(address) -
                                             reinterpret_cast<UINT_PTR>(module)));
  } else {
    std::snprintf(out, size, "0x%p (not in a module)", address);
  }
}

LONG CALLBACK Handler(EXCEPTION_POINTERS* info) {
  const EXCEPTION_RECORD* record = info->ExceptionRecord;
  switch (record->ExceptionCode) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_IN_PAGE_ERROR:
      break;
    default:
      return EXCEPTION_CONTINUE_SEARCH;  // C++ exceptions, debug output and the like
  }
  // A handful is plenty, and a crash loop must not fill the disk.
  if (g_logged.fetch_add(1) >= 6) return EXCEPTION_CONTINUE_SEARCH;

  char where[MAX_PATH + 32];
  Describe(record->ExceptionAddress, where, sizeof(where));
  if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
    const char* action = record->ExceptionInformation[0] == 0 ? "reading"
                         : record->ExceptionInformation[0] == 1 ? "writing" : "executing";
    log::Error("EXCEPTION 0x%08lX at %s, %s address 0x%p (thread %lu)", record->ExceptionCode, where,
               action, reinterpret_cast<void*>(record->ExceptionInformation[1]), GetCurrentThreadId());
  } else {
    log::Error("EXCEPTION 0x%08lX at %s (thread %lu)", record->ExceptionCode, where,
               GetCurrentThreadId());
  }
#if defined(_M_IX86) || defined(__i386__)
  // Return addresses still on the stack say how it got there.
  char line[600] = "";
  const UINT_PTR* frame = reinterpret_cast<const UINT_PTR*>(info->ContextRecord->Ebp);
  for (int depth = 0; depth < 6 && frame && !IsBadReadPtr(frame, 2 * sizeof(UINT_PTR)); ++depth) {
    char caller[MAX_PATH + 32];
    Describe(reinterpret_cast<const void*>(frame[1]), caller, sizeof(caller));
    if (std::strlen(line) + std::strlen(caller) + 4 >= sizeof(line)) break;
    if (depth) std::strcat(line, " < ");
    std::strcat(line, caller);
    const UINT_PTR* next = reinterpret_cast<const UINT_PTR*>(frame[0]);
    if (next <= frame) break;
    frame = next;
  }
  if (line[0]) log::Error("  called from: %s", line);
#elif defined(_M_X64)
  // The same, the way 64-bit code has to be walked: by each function's
  // unwind record, on a copy so the real context goes on untouched.
  char line[900] = "";
  CONTEXT context = *info->ContextRecord;
  for (int depth = 0; depth < 8; ++depth) {
    DWORD64 imageBase = 0;
    if (PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr)) {
      void* handlerData = nullptr;
      DWORD64 establisher = 0;
      RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, function, &context, &handlerData,
                       &establisher, nullptr);
    } else {
      // No record means a leaf: its caller's address is what the stack points at.
      if (IsBadReadPtr(reinterpret_cast<const void*>(context.Rsp), sizeof(DWORD64))) break;
      context.Rip = *reinterpret_cast<const DWORD64*>(context.Rsp);
      context.Rsp += sizeof(DWORD64);
    }
    if (!context.Rip) break;
    char caller[MAX_PATH + 32];
    Describe(reinterpret_cast<const void*>(context.Rip), caller, sizeof(caller));
    if (std::strlen(line) + std::strlen(caller) + 4 >= sizeof(line)) break;
    if (depth) std::strcat(line, " < ");
    std::strcat(line, caller);
  }
  if (line[0]) log::Error("  called from: %s", line);
#endif
  return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

void Install() { AddVectoredExceptionHandler(1, Handler); }

}  // namespace csp::crashlog
