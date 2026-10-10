// Opens (or creates) the shared block. Either game may start first: whoever
// gets there first initialises the header, the other just validates it.
#pragma once

#include "csp_protocol.h"

namespace csp {

class SharedMapping {
 public:
  SharedMapping() = default;
  ~SharedMapping();
  SharedMapping(const SharedMapping&) = delete;
  SharedMapping& operator=(const SharedMapping&) = delete;

  // Logs and returns false on failure (e.g. protocol version mismatch).
  bool Open(Side self);
  void Close();

  SharedBlock* block() const { return block_; }

  // From any thread: "this process is here".
  void Beat();
  // From the game's own thread, once a frame: "and the game is running".
  void Frame();

  bool PeerAlive() const;
  // The peer's own count of its frames, for telling a running game from a
  // process that is merely still there.
  uint32_t PeerFrames() const;

 private:
  void* handle_ = nullptr;
  SharedBlock* block_ = nullptr;
  Side self_ = kSideSpyro;
};

// The ground mapping (csp_protocol.h). Spyro publishes, Crash reads.
class GroundMapping {
 public:
  GroundMapping() = default;
  ~GroundMapping();
  GroundMapping(const GroundMapping&) = delete;
  GroundMapping& operator=(const GroundMapping&) = delete;

  bool Open();
  void Close();
  bool open() const { return header_ != nullptr; }

  // Spyro: `fields` says what the page is (its seq and triangle count are
  // filled in here), `corners` is nine floats a triangle.
  void Publish(const GroundPage& fields, const float* corners, uint32_t triangles);

  // Crash: the generation of the latest whole page, 0 if there is none.
  uint32_t Latest() const;
  // Crash: copies the latest page out. False if there is none, or Spyro
  // rewrote it meanwhile (ask again next frame).
  bool Read(GroundPage& fields, float* corners, uint32_t room) const;

 private:
  GroundPage* Page(uint32_t index) const;
  void* handle_ = nullptr;
  GroundHeader* header_ = nullptr;
};

}  // namespace csp
