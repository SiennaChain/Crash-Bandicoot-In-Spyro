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

GroundMapping::~GroundMapping() { Close(); }

bool GroundMapping::Open() {
  handle_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, kGroundMappingSize, kGroundMappingName);
  if (!handle_) {
    log::Error("ground: CreateFileMapping failed: %lu", GetLastError());
    return false;
  }
  header_ = static_cast<GroundHeader*>(MapViewOfFile(handle_, FILE_MAP_ALL_ACCESS, 0, 0, kGroundMappingSize));
  if (!header_) {
    log::Error("ground: MapViewOfFile failed: %lu", GetLastError());
    Close();
    return false;
  }
  uint32_t expected = 0;
  if (header_->magic.compare_exchange_strong(expected, kMagicInitializing)) {
    header_->version = kGroundVersion;
    header_->size = kGroundMappingSize;
    header_->latest.store(kGroundNone);
    header_->magic.store(kGroundMagic, std::memory_order_release);
  } else {
    for (int i = 0; i < 1000 && header_->magic.load(std::memory_order_acquire) != kGroundMagic; ++i) Sleep(1);
    if (header_->magic.load(std::memory_order_acquire) != kGroundMagic || header_->version != kGroundVersion) {
      log::Error("ground: the mapping is not one this bridge understands. Rebuild both bridges.");
      Close();
      return false;
    }
  }
  return true;
}

void GroundMapping::Close() {
  if (header_) {
    UnmapViewOfFile(header_);
    header_ = nullptr;
  }
  if (handle_) {
    CloseHandle(handle_);
    handle_ = nullptr;
  }
}

GroundPage* GroundMapping::Page(uint32_t index) const {
  return reinterpret_cast<GroundPage*>(reinterpret_cast<char*>(header_) + kGroundPagesAt + index * kGroundPageBytes);
}

void GroundMapping::Publish(const GroundPage& fields, const float* corners, uint32_t triangles) {
  if (!header_) return;
  if (triangles > kGroundMaxTriangles) triangles = kGroundMaxTriangles;
  const uint32_t latest = header_->latest.load(std::memory_order_acquire);
  const uint32_t index = latest == 0 ? 1 : 0;
  GroundPage* page = Page(index);
  const uint32_t seq = page->seq.load(std::memory_order_relaxed);
  page->seq.store(seq + 1, std::memory_order_release);
  page->generation = fields.generation;
  page->triangles = triangles;
  page->crashGeneration = fields.crashGeneration;
  page->world = fields.world;
  page->centre = fields.centre;
  page->reach = fields.reach;
  page->spyroAnchor = fields.spyroAnchor;
  page->scale = fields.scale;
  page->crashAnchor = fields.crashAnchor;
  std::memcpy(page + 1, corners, static_cast<size_t>(triangles) * 9 * sizeof(float));
  page->seq.store(seq + 2, std::memory_order_release);
  header_->latest.store(index, std::memory_order_release);
}

uint32_t GroundMapping::Latest() const {
  if (!header_) return 0;
  const uint32_t index = header_->latest.load(std::memory_order_acquire);
  return index > 1 ? 0 : Page(index)->generation;
}

bool GroundMapping::Read(GroundPage& fields, float* corners, uint32_t room) const {
  if (!header_) return false;
  const uint32_t index = header_->latest.load(std::memory_order_acquire);
  if (index > 1) return false;
  const GroundPage* page = Page(index);
  const uint32_t before = page->seq.load(std::memory_order_acquire);
  if (before & 1) return false;
  std::memcpy(static_cast<void*>(&fields), page, sizeof(GroundPage));
  if (fields.triangles > room || fields.triangles > kGroundMaxTriangles) return false;
  std::memcpy(corners, page + 1, static_cast<size_t>(fields.triangles) * 9 * sizeof(float));
  return page->seq.load(std::memory_order_acquire) == before;
}

}  // namespace csp
