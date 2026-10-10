// Minimal thread-safe file logger. Each bridge writes next to its DLL
// (crashspyro_crash.log / crashspyro_spyro.log) because neither game has a console we
// can rely on.
#pragma once

#include <string>

namespace csp::log {

void Init(const std::wstring& path);
void Info(const char* fmt, ...);
void Error(const char* fmt, ...);

}  // namespace csp::log
