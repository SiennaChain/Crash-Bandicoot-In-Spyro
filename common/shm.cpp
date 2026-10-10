#include "shm.h"

#include <windows.h>

#include "log.h"

namespace csp {

SharedMapping::~SharedMapping() { Close(); }

bool SharedMapping::Open(Side self) {
  self_ = self;
  handle_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, kMappingSize,
                               kMappingName);
  if (!handle_) {
    log::Error("CreateFileMapping failed: %lu", GetLastError());
    return false;
  }
  block_ = static_cast<SharedBlock*>(
      MapViewOfFile(handle_, FILE_MAP_ALL_ACCESS, 0, 0, kMappingSize));
  if (!block_) {
    log::Error("MapViewOfFile failed: %lu", GetLastError());
    Close();
    return false;
  }

  // Page-file mappings start zeroed, so magic == 0 means nobody has
  // initialised the header yet.
  Header& h = block_->header;
  uint32_t expected = 0;
  if (h.magic.compare_exchange_strong(expected, kMagicInitializing)) {
    h.version = kVersion;
    h.size = kMappingSize;
    h.magic.store(kMagic, std::memory_order_release);
    log::Info("created shared block %ls (protocol v%u)", kMappingName, kVersion);
  } else {
    for (int i = 0; i < 1000 && h.magic.load(std::memory_order_acquire) != kMagic; ++i) {
      Sleep(1);
    }
    if (h.magic.load(std::memory_order_acquire) != kMagic) {
      log::Error("shared block has bad magic 0x%08X", h.magic.load());
      Close();
      return false;
    }
    if (h.version != kVersion) {
      log::Error("protocol mismatch: peer is v%u, we are v%u. Rebuild both bridges.", h.version,
                 kVersion);
      Close();
      return false;
    }
    log::Info("joined shared block %ls (protocol v%u)", kMappingName, kVersion);
  }

  h.peers[self_].pid.store(GetCurrentProcessId());
  Beat();
  return true;
}

void SharedMapping::Close() {
  if (block_) {
    block_->header.peers[self_].pid.store(0);
    UnmapViewOfFile(block_);
    block_ = nullptr;
  }
  if (handle_) {
    CloseHandle(handle_);
    handle_ = nullptr;
  }
}

void SharedMapping::Beat() {
  PeerInfo& me = block_->header.peers[self_];
  me.heartbeatMs.store(GetTickCount(), std::memory_order_relaxed);
  me.beats.fetch_add(1, std::memory_order_release);
}

void SharedMapping::Frame() {
  block_->header.peers[self_].frames.fetch_add(1, std::memory_order_release);
}

bool SharedMapping::PeerAlive() const {
  const PeerInfo& peer = block_->header.peers[Other(self_)];
  const uint32_t beats = peer.beats.load(std::memory_order_acquire);
  return peer.pid.load() != 0 &&
         HeartbeatAlive(peer.heartbeatMs.load(std::memory_order_relaxed), beats, GetTickCount());
}

uint32_t SharedMapping::PeerFrames() const {
  return block_->header.peers[Other(self_)].frames.load(std::memory_order_acquire);
}

}  // namespace csp
