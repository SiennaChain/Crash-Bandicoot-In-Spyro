#include "memory.h"

#include <windows.h>

#include <cstring>

namespace csp::mem {

uintptr_t ImageBase() {
  static const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
  return base;
}

uint32_t ImageSize() {
  static const uint32_t size = [] {
    const auto* base = reinterpret_cast<const BYTE*>(ImageBase());
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    return static_cast<uint32_t>(nt->OptionalHeader.SizeOfImage);
  }();
  return size;
}

bool Read(uintptr_t address, void* out, size_t size) {
  SIZE_T done = 0;
  return address != 0 &&
         ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(address), out, size,
                           &done) &&
         done == size;
}

bool Write(uintptr_t address, const void* data, size_t size) {
  SIZE_T done = 0;
  return address != 0 &&
         WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<LPVOID>(address), data, size,
                            &done) &&
         done == size;
}

uintptr_t Pointer(uintptr_t address) {
  uintptr_t value = 0;
  if (!Read(address, value)) return 0;
  if (value < 0x10000 || value > 0x00007FFFFFFFFFFFull || (value & 7) != 0) return 0;
  return value;
}

bool IsA(uintptr_t object, uint32_t vtableRva) {
  uintptr_t vtable = 0;
  return object != 0 && Read(object, vtable) && vtable == ImageBase() + vtableRva;
}

bool StartsWith(uintptr_t address, const char* hex) {
  const auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  BYTE expected[64];
  size_t count = 0;
  for (; hex[0] && hex[1] && count < sizeof(expected); hex += 2) {
    const int high = nibble(hex[0]), low = nibble(hex[1]);
    if (high < 0 || low < 0) return false;
    expected[count++] = static_cast<BYTE>(high << 4 | low);
  }
  BYTE actual[sizeof(expected)];
  return count != 0 && Read(address, actual, count) && std::memcmp(actual, expected, count) == 0;
}

bool IsExecutable(uintptr_t address) {
  MEMORY_BASIC_INFORMATION info = {};
  if (!VirtualQuery(reinterpret_cast<LPCVOID>(address), &info, sizeof(info))) return false;
  const DWORD executable = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
  return info.State == MEM_COMMIT && (info.Protect & executable) != 0;
}

namespace {

// Writes one pointer-sized slot that normally lives in read-only memory.
bool WriteSlot(void** slot, void* value) {
  DWORD protection = 0;
  // Executable as well as writable: the slot can share a page with code that
  // another thread is running right now.
  if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &protection)) return false;
  *slot = value;
  VirtualProtect(slot, sizeof(void*), protection, &protection);
  return true;
}

}  // namespace

void* PatchImport(const char* dll, const char* name, void* replacement) {
  const HMODULE provider = GetModuleHandleA(dll);
  void* target = provider ? reinterpret_cast<void*>(GetProcAddress(provider, name)) : nullptr;
  if (!target) return nullptr;

  auto* base = reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr));
  auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
  auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  if (!dir.VirtualAddress) return nullptr;

  for (auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
       desc->Name; ++desc) {
    if (_stricmp(reinterpret_cast<const char*>(base + desc->Name), dll) != 0) continue;
    // The slot is found by the name the exe asks for, where it keeps its list
    // of them, and only failing that by what is in the slot: something else
    // loaded into the game may have put a routine of its own there already,
    // and then it is that routine ours has to call on to.
    auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);
    auto* asked = desc->OriginalFirstThunk ? reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->OriginalFirstThunk) : nullptr;
    for (; thunk->u1.Function; ++thunk) {
      bool found = reinterpret_cast<void*>(thunk->u1.Function) == target;
      if (asked) {
        if (!asked->u1.AddressOfData) break;
        if (!found && !IMAGE_SNAP_BY_ORDINAL(asked->u1.Ordinal)) {
          const auto* by = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + asked->u1.AddressOfData);
          found = std::strcmp(reinterpret_cast<const char*>(by->Name), name) == 0;
        }
        ++asked;
      }
      if (!found) continue;
      void* previous = reinterpret_cast<void*>(thunk->u1.Function);
      if (previous == replacement) return nullptr;
      return WriteSlot(reinterpret_cast<void**>(&thunk->u1.Function), replacement) ? previous : nullptr;
    }
  }
  return nullptr;
}

void* PatchVtable(void* object, int index, void* replacement) {
  void** vtable = *reinterpret_cast<void***>(object);
  void* previous = vtable[index];
  if (previous == replacement) return nullptr;
  return WriteSlot(&vtable[index], replacement) ? previous : nullptr;
}

}  // namespace csp::mem
