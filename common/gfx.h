// Direct3D 11, which both games draw with: getting called when a frame is
// about to be shown, and the few things needed to draw something of the
// bridge's own at that moment without the game noticing (docs/PHASE2.md).
#pragma once

#include <d3d11.h>
#include <dxgi.h>

namespace csp::gfx {

// On the thread that presents, just before each frame is shown.
using PresentFn = void (*)(IDXGISwapChain* chain);
// On the thread that draws, just before a depth buffer is cleared, with
// the depth it is being cleared to.
using ClearDepthFn = void (*)(ID3D11DeviceContext* context, ID3D11DepthStencilView* view, float clearedTo);

// Hooks the two. False if Direct3D is not ready to be asked yet (call again
// later); true once the hooks are in. `onClearDepth` may be null.
bool Install(PresentFn onPresent, ClearDepthFn onClearDepth);

// While on, the game's frames are shown as they come, without waiting for
// the screen's next refresh as the game asked. For a game whose own window
// nobody is looking at.
void NoWait(bool on);

// Compiles a shader; null (and a line in the log) if it will not.
ID3DBlob* Compile(const char* source, const char* entry, const char* target);

// Everything about the context a drawing of the bridge's disturbs, put back
// as it was when this goes out of scope. The games keep their own note of
// what they have set and do not set it again.
class SavedState {
 public:
  explicit SavedState(ID3D11DeviceContext* context);
  ~SavedState();
  SavedState(const SavedState&) = delete;
  SavedState& operator=(const SavedState&) = delete;

 private:
  ID3D11DeviceContext* context_;
  ID3D11RenderTargetView* targets_[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
  ID3D11DepthStencilView* depth_ = nullptr;
  UINT viewportCount_ = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
  D3D11_VIEWPORT viewports_[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
  ID3D11InputLayout* layout_ = nullptr;
  D3D11_PRIMITIVE_TOPOLOGY topology_ = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
  ID3D11VertexShader* vs_ = nullptr;
  ID3D11PixelShader* ps_ = nullptr;
  ID3D11GeometryShader* gs_ = nullptr;
  ID3D11HullShader* hs_ = nullptr;
  ID3D11DomainShader* ds_ = nullptr;
  ID3D11ShaderResourceView* resources_[2] = {};
  ID3D11SamplerState* samplers_[2] = {};
  ID3D11Buffer* constants_ = nullptr;
  ID3D11BlendState* blend_ = nullptr;
  FLOAT blendFactor_[4] = {};
  UINT sampleMask_ = 0;
  ID3D11DepthStencilState* depthState_ = nullptr;
  UINT stencilRef_ = 0;
  ID3D11RasterizerState* raster_ = nullptr;
};

// The vertex shader every drawing here uses: one triangle that covers the
// whole target, with `uv` running 0..1 across and down it. Draw(3, 0), no
// vertex buffer, no input layout.
extern const char kWholeTargetVertexShader[];

}  // namespace csp::gfx
