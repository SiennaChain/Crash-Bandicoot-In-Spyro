// crashspyro.ini lives next to the bridge DLL in each game's folder. What
// was found in each game (addresses, offsets) can be overridden there while
// reverse engineering without rebuilding, and (via Changed()) without
// restarting the game.
#pragma once

#include <cstdint>
#include <string>

namespace csp {

class Config {
 public:
  explicit Config(std::wstring path) : path_(std::move(path)) {}

  const std::wstring& path() const { return path_; }
  std::string GetString(const char* section, const char* key, const char* fallback = "") const;
  int GetInt(const char* section, const char* key, int fallback) const;
  float GetFloat(const char* section, const char* key, float fallback) const;
  // Decimal or 0x hex. For offsets and addresses relative to the exe.
  uint32_t GetOffset(const char* section, const char* key, uint32_t fallback) const;

  // True on the first call, and again each time the file has been saved
  // since the previous call. The getters always read from disk; this only
  // tells a caller when its own copies of the values are out of date.
  bool Changed();

 private:
  std::wstring path_;
  uint64_t lastWrite_ = ~uint64_t{0};
};

}  // namespace csp
