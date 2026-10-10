#include "session.h"

#include "csp_protocol.h"

namespace csp::session {
namespace {

// Both games are 64-bit, but the two numbers sit in the same place in a
// 32-bit header up to and including SizeOfImage, so one reading does for an
// exe of either kind on disk.
ExeId FromHeader(const BYTE* base) {
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
  return {nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage};
}

}  // namespace

bool OfFile(const std::wstring& path, ExeId* id) {
  const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  BYTE head[0x1000] = {};
  DWORD got = 0;
  const bool read = ReadFile(file, head, sizeof(head), &got, nullptr) != 0;
  CloseHandle(file);
  if (!read || got < sizeof(IMAGE_DOS_HEADER)) return false;
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(head);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 || dos->e_lfanew + sizeof(IMAGE_NT_HEADERS64) > got) return false;
  if (reinterpret_cast<const IMAGE_NT_HEADERS64*>(head + dos->e_lfanew)->Signature != IMAGE_NT_SIGNATURE) return false;
  *id = FromHeader(head);
  return true;
}

HANDLE Hold() {
  const HANDLE held = CreateMutexW(nullptr, FALSE, kSessionName);
  if (held && GetLastError() == ERROR_ALREADY_EXISTS) {
    CloseHandle(held);
    return nullptr;
  }
  return held;
}

Verdict Decide(const Game& game, const std::wstring& ini) {
  wchar_t path[MAX_PATH] = {};
  const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
  const wchar_t* name = path;
  for (DWORD i = 0; i < length; ++i) {
    if (path[i] == L'\\' || path[i] == L'/') name = path + i + 1;
  }
  if (lstrcmpiW(name, game.exe) != 0) return Verdict::kNotThisExe;

  if (GetPrivateProfileIntW(L"bridge", L"without_launcher", 0, ini.c_str()) == 0) {
    const HANDLE held = OpenMutexW(SYNCHRONIZE, FALSE, kSessionName);
    if (!held) return Verdict::kNoLauncher;
    CloseHandle(held);
  }

  if (GetPrivateProfileIntW(L"bridge", L"any_exe", 0, ini.c_str()) == 0 &&
      FromHeader(reinterpret_cast<const BYTE*>(GetModuleHandleW(nullptr))) != game.id) {
    return Verdict::kOtherBuild;
  }
  return Verdict::kRun;
}

}  // namespace csp::session
