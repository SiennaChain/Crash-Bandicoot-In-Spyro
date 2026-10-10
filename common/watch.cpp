#include "watch.h"

#include <windows.h>

#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>

#include "log.h"
#include "memory.h"

namespace csp::watch {
namespace {

constexpr int kPlaces = 48, kCallers = 4;
struct Place {
  uintptr_t at;                // the instruction after the one that wrote
  uintptr_t callers[kCallers];
  uint32_t count;
  uint32_t thread;
};
Place g_places[kPlaces];
int g_placeCount = 0;
uint32_t g_missed = 0;         // writes from places there was no room to note
std::atomic_flag g_busy = ATOMIC_FLAG_INIT;
std::atomic<bool> g_armed{false};

// On the thread that wrote, straight after the write. Nothing here may wait
// on anything the game might be holding: no logging, no allocation.
LONG CALLBACK OnStep(EXCEPTION_POINTERS* info) {
  if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
  CONTEXT* context = info->ContextRecord;
  if (!(context->Dr6 & 1) || !g_armed.load(std::memory_order_relaxed)) return EXCEPTION_CONTINUE_SEARCH;
  context->Dr6 = 0;

  const uintptr_t at = context->Rip;
  while (g_busy.test_and_set(std::memory_order_acquire)) {
  }
  Place* place = nullptr;
  for (int i = 0; i < g_placeCount; ++i) {
    if (g_places[i].at == at) place = &g_places[i];
  }
  if (!place && g_placeCount < kPlaces) {
    place = &g_places[g_placeCount++];
    *place = Place{};
    place->at = at;
    place->thread = GetCurrentThreadId();
    // What called it, by each function's unwind record, on a copy.
    CONTEXT copy = *context;
    for (int depth = 0; depth < kCallers; ++depth) {
      DWORD64 imageBase = 0;
      if (PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(copy.Rip, &imageBase, nullptr)) {
        void* handlerData = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, copy.Rip, function, &copy, &handlerData, &establisher, nullptr);
      } else {
        uintptr_t back = 0;
        if (!mem::Read(copy.Rsp, back)) break;
        copy.Rip = back;
        copy.Rsp += sizeof(DWORD64);
      }
      if (!copy.Rip) break;
      place->callers[depth] = copy.Rip;
    }
  }
  if (place) ++place->count;
  else ++g_missed;
  g_busy.clear(std::memory_order_release);
  return EXCEPTION_CONTINUE_EXECUTION;
}

// Sets or clears the first data breakpoint on every thread but this one.
int OnEveryThread(uintptr_t address, int size, bool arm) {
  const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return 0;
  const DWORD process = GetCurrentProcessId(), self = GetCurrentThreadId();
  // Length, as the processor wants it: 1, 2, 8 and 4 bytes are 0, 1, 2 and 3.
  const DWORD64 length = size == 2 ? 1 : size == 8 ? 2 : size == 4 ? 3 : 0;
  int done = 0;
  THREADENTRY32 entry{};
  entry.dwSize = sizeof(entry);
  for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
    if (entry.th32OwnerProcessID != process || entry.th32ThreadID == self) continue;
    const HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, entry.th32ThreadID);
    if (!thread) continue;
    if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
      CONTEXT context{};
      context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
      if (GetThreadContext(thread, &context)) {
        context.Dr7 &= ~((DWORD64{3} << 0) | (DWORD64{0xF} << 16));  // breakpoint 0: off, no kind, no length
        if (arm) {
          context.Dr0 = address;
          context.Dr6 = 0;
          context.Dr7 |= 1 | (DWORD64{1} << 16) | (length << 18);    // on for this thread; on writes; so many bytes
        } else {
          context.Dr0 = 0;
        }
        if (SetThreadContext(thread, &context)) ++done;
      }
      ResumeThread(thread);
    }
    CloseHandle(thread);
  }
  CloseHandle(snapshot);
  return done;
}

void Describe(uintptr_t address, char* out, size_t size) {
  if (mem::InImage(address)) std::snprintf(out, size, "exe+0x%llX", static_cast<unsigned long long>(address - mem::ImageBase()));
  else std::snprintf(out, size, "0x%llX", static_cast<unsigned long long>(address));
}

}  // namespace

void Writers(uintptr_t address, int size, int seconds) {
  static bool installed = false;
  if (!installed) {
    AddVectoredExceptionHandler(1, OnStep);
    installed = true;
  }
  if (size != 1 && size != 2 && size != 4 && size != 8) size = 4;
  address &= ~static_cast<uintptr_t>(size - 1);  // the processor wants it lined up with its size
  g_placeCount = 0;
  g_missed = 0;
  g_armed.store(true);
  const int threads = OnEveryThread(address, size, true);
  log::Info("watch: watching %d bytes at %p for writes on %d threads for %d s", size, reinterpret_cast<void*>(address), threads, seconds);
  Sleep(static_cast<DWORD>(seconds) * 1000);
  OnEveryThread(address, size, false);
  Sleep(50);  // let a stop already on its way arrive
  g_armed.store(false);

  std::sort(g_places, g_places + g_placeCount, [](const Place& a, const Place& b) { return a.count > b.count; });
  uint32_t total = g_missed;
  for (int i = 0; i < g_placeCount; ++i) total += g_places[i].count;
  log::Info("watch: %u writes in %d s from %d places%s", total, seconds, g_placeCount, g_missed ? " (and more there was no room to note)" : "");
  for (int i = 0; i < g_placeCount; ++i) {
    char line[400], part[64];
    Describe(g_places[i].at, part, sizeof(part));
    std::snprintf(line, sizeof(line), "%6u  before %s (thread %u)", g_places[i].count, part, g_places[i].thread);
    for (const uintptr_t caller : g_places[i].callers) {
      if (!caller) break;
      Describe(caller, part, sizeof(part));
      std::strncat(line, " < ", sizeof(line) - std::strlen(line) - 1);
      std::strncat(line, part, sizeof(line) - std::strlen(line) - 1);
    }
    log::Info("watch: %s", line);
  }
}

}  // namespace csp::watch
