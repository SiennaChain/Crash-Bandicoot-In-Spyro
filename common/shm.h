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

}  // namespace csp
