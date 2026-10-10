// Safe reads/writes of the host game's own memory. Goes through
// Read/WriteProcessMemory on the current process so a wrong address or a
// pointer the game freed a moment ago fails cleanly instead of crashing the
// game. Both games are 64-bit.
#pragma once

#include <cstddef>
#include <cstdint>

namespace csp::mem {

// Where the game's exe is loaded, and how much memory it takes up there.
uintptr_t ImageBase();
uint32_t ImageSize();
inline bool InImage(uintptr_t address) {
  return address >= ImageBase() && address < ImageBase() + ImageSize();
}

bool Read(uintptr_t address, void* out, size_t size);
bool Write(uintptr_t address, const void* data, size_t size);

template <typename T>
bool Read(uintptr_t address, T& out) {
  return Read(address, &out, sizeof(T));
}

// The pointer stored at `address`, or 0 if it cannot be read or is plainly
// not a pointer to an object (null, low, unaligned, or outside user space).
uintptr_t Pointer(uintptr_t address);

// Whether the object at `object` is of the class whose table of virtual
// functions sits `vtableRva` bytes into the exe. The cheapest proof there is
// that a pointer followed from a known place still points at what it did
// when the place was found.
bool IsA(uintptr_t object, uint32_t vtableRva);

// Whether the code at `address` begins with the bytes written out in `hex`
// ("48895c2408..."). An address is only as good as the build it was found
// in, and both exes are stored encrypted and unpacked as they start, so this
// is also how a bridge knows the code is there yet.
bool StartsWith(uintptr_t address, const char* hex);

// Whether `address` is in memory that can be run. Steam's wrapper unpacks an
// exe's code into memory it can write to and only afterwards makes that
// memory code again; the bytes being right does not yet mean they can be
// hooked.
bool IsExecutable(uintptr_t address);

// Redirects the game exe's import of `name` from `dll` to `replacement`.
// Returns what the exe was calling until now, for the replacement to call
// through to, or null if the exe does not import it.
void* PatchImport(const char* dll, const char* name, void* replacement);

// Replaces one method of an object by overwriting its vtable slot, which
// affects every object of that class. Returns the previous method, or null if
// it was already `replacement` or could not be written.
void* PatchVtable(void* object, int index, void* replacement);

}  // namespace csp::mem
