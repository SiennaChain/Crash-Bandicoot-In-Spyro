#include "ue.h"

#include <cstring>
#include <initializer_list>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "memory.h"

namespace csp::ue {
namespace {

// TNameEntryArray: 128 pointers to chunks of 16384 pointers to entries, then
// how many names there are.
constexpr uint32_t kNamesCount = 0x400;
constexpr uint32_t kChunkSize = 16384;
// FNameEntry: its number (doubled, low bit set if the text is wide), a
// pointer used by the engine's hash table, then the text.
constexpr uint32_t kEntryText = 0x10;

uint32_t g_namesRva = 0;
std::mutex g_mutex;
std::unordered_map<int32_t, std::string> g_cache;

uintptr_t Names() { return g_namesRva ? mem::Pointer(mem::ImageBase() + g_namesRva) : 0; }

// The text of the entry at `entry`. False for none, and for wide names:
// nothing the bridge looks for has one.
bool EntryText(uintptr_t entry, char* out, size_t size) {
  int32_t index = 0;
  if (!entry || !mem::Read(entry, index) || (index & 1)) return false;
  // An entry is only as long as its text, so a long read can run off the
  // end of the memory it is in. Short first, the rest only if needed.
  size_t have = 0;
  for (const size_t step : {size_t{24}, size - 1}) {
    if (step <= have) break;
    if (!mem::Read(entry + kEntryText + have, out + have, step - have)) {
      if (have == 0) return false;
      break;
    }
    have = step;
    if (std::memchr(out, 0, have)) return true;
  }
  out[have] = 0;
  return have != 0;
}

uintptr_t EntryAt(uintptr_t names, int32_t index) {
  if (!names || index < 0) return 0;
  const uintptr_t chunk = mem::Pointer(names + (index / kChunkSize) * sizeof(uintptr_t));
  return chunk ? mem::Pointer(chunk + (index % kChunkSize) * sizeof(uintptr_t)) : 0;
}

// The first field called `name` among `owner`'s own (a class's functions and
// properties, or a function's parameters).
uintptr_t ChildNamed(uintptr_t owner, const char* name) {
  uintptr_t field = mem::Pointer(owner + kStructChildren);
  for (int guard = 0; field && guard < 4096; ++guard) {
    if (NameOfObject(field) == name) return field;
    field = mem::Pointer(field + kFieldNext);
  }
  return 0;
}

// The same, looking through `type` and then everything it is built on.
uintptr_t MemberNamed(uintptr_t type, const char* name) {
  for (int depth = 0; type && depth < 64; ++depth) {
    if (const uintptr_t found = ChildNamed(type, name)) return found;
    type = mem::Pointer(type + kStructSuper);
  }
  return 0;
}

}  // namespace

void Init(uint32_t namesRva) { g_namesRva = namesRva; }

int32_t FindName(const char* text) {
  const uintptr_t names = Names();
  int32_t count = 0;
  if (!names || !mem::Read(names + kNamesCount, count) || count <= 0 || count > 4 * 1024 * 1024) return -1;
  const size_t length = std::strlen(text);
  std::vector<uintptr_t> chunk(kChunkSize);
  char name[64];
  if (length == 0 || length >= sizeof(name)) return -1;
  for (int32_t first = 0; first < count; first += kChunkSize) {
    const uintptr_t at = mem::Pointer(names + (first / kChunkSize) * sizeof(uintptr_t));
    if (!at || !mem::Read(at, chunk.data(), chunk.size() * sizeof(uintptr_t))) continue;
    for (uint32_t i = 0; i < kChunkSize && first + static_cast<int32_t>(i) < count; ++i) {
      // Only the length of what is looked for, and one more for the end.
      if (!chunk[i] || !mem::Read(chunk[i] + kEntryText, name, length + 1)) continue;
      int32_t index = 0;
      if (name[length] == 0 && std::memcmp(name, text, length) == 0 && mem::Read(chunk[i], index) && !(index & 1)) {
        return first + static_cast<int32_t>(i);
      }
    }
  }
  return -1;
}

std::string NameOf(int32_t index) {
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto found = g_cache.find(index);
    if (found != g_cache.end()) return found->second;
  }
  char text[128];
  if (!EntryText(EntryAt(Names(), index), text, sizeof(text))) return {};
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_cache.emplace(index, text).first->second;
}

std::string NameOfObject(uintptr_t object) {
  int32_t index = -1;
  return object && mem::Read(object + kObjectName, index) ? NameOf(index) : std::string();
}

uintptr_t ClassOf(uintptr_t object) { return object ? mem::Pointer(object + kObjectClass) : 0; }

uintptr_t FindFunction(uintptr_t object, const char* name) { return MemberNamed(ClassOf(object), name); }

int32_t FindParam(uintptr_t function, const char* name) {
  int32_t offset = -1;
  const uintptr_t parameter = function ? ChildNamed(function, name) : 0;
  return parameter && mem::Read(parameter + kPropertyOffset, offset) ? offset : -1;
}

int32_t FindProperty(uintptr_t object, const char* name) {
  int32_t offset = -1;
  const uintptr_t property = MemberNamed(ClassOf(object), name);
  return property && mem::Read(property + kPropertyOffset, offset) ? offset : -1;
}

}  // namespace csp::ue
