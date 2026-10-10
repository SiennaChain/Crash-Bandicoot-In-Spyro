#include "gfx.h"

#include <atomic>
#include <cstring>

#include "hook.h"
#include "log.h"

namespace csp::gfx {

const char kWholeTargetVertexShader[] = R"(
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
V vs(uint id : SV_VertexID) {
  V o;
  float2 t = float2((id << 1) & 2, id & 2);
  o.pos = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
  o.uv = t;
  return o;
}
)";

namespace {

using PresentReal = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain* chain, UINT interval, UINT flags);
using ClearDepthReal = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext* context, ID3D11DepthStencilView* view, UINT flags, FLOAT depth, UINT8 stencil);

PresentReal g_present = nullptr;
ClearDepthReal g_clearDepth = nullptr;
PresentFn g_onPresent = nullptr;
ClearDepthFn g_onClearDepth = nullptr;

HRESULT STDMETHODCALLTYPE OnPresent(IDXGISwapChain* chain, UINT interval, UINT flags) {
  if (g_onPresent && !(flags & DXGI_PRESENT_TEST)) g_onPresent(chain);
  return g_present(chain, interval, flags);
}

void STDMETHODCALLTYPE OnClearDepth(ID3D11DeviceContext* context, ID3D11DepthStencilView* view, UINT flags, FLOAT depth, UINT8 stencil) {
  if (g_onClearDepth && view && (flags & D3D11_CLEAR_DEPTH)) g_onClearDepth(context, view, depth);
  g_clearDepth(context, view, flags, depth, stencil);
}

}  // namespace

bool Install(PresentFn onPresent, ClearDepthFn onClearDepth) {
  static bool installed = false;
  if (installed) return true;
  if (!GetModuleHandleW(L"d3d11.dll") || !GetModuleHandleW(L"dxgi.dll")) return false;

  // The routines are found in the tables of a device and swap chain of the
  // bridge's own, made for a window nobody sees and let go again at once:
  // every device in the process shares them.
  WNDCLASSEXW windowClass{};
  windowClass.cbSize = sizeof(windowClass);
  windowClass.lpfnWndProc = DefWindowProcW;
  windowClass.hInstance = GetModuleHandleW(nullptr);
  windowClass.lpszClassName = L"CrashSpyroGfxProbe";
  RegisterClassExW(&windowClass);
  const HWND window = CreateWindowExW(0, windowClass.lpszClassName, L"", WS_OVERLAPPED, 0, 0, 16, 16, nullptr, nullptr, windowClass.hInstance, nullptr);
  if (!window) return false;

  DXGI_SWAP_CHAIN_DESC description{};
  description.BufferCount = 1;
  description.BufferDesc.Width = 16;
  description.BufferDesc.Height = 16;
  description.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  description.OutputWindow = window;
  description.SampleDesc.Count = 1;
  description.Windowed = TRUE;
  IDXGISwapChain* chain = nullptr;
  ID3D11Device* device = nullptr;
  ID3D11DeviceContext* context = nullptr;
  const HRESULT made = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &description,
                                                     &chain, &device, nullptr, &context);
  void* present = nullptr;
  void* clearDepth = nullptr;
  if (SUCCEEDED(made) && chain && context) {
    present = (*reinterpret_cast<void***>(chain))[8];       // IDXGISwapChain::Present
    clearDepth = (*reinterpret_cast<void***>(context))[53];  // ID3D11DeviceContext::ClearDepthStencilView
  }
  if (context) context->Release();
  if (device) device->Release();
  if (chain) chain->Release();
  DestroyWindow(window);
  UnregisterClassW(windowClass.lpszClassName, windowClass.hInstance);
  if (!present || !clearDepth) {
    log::Error("gfx: could not make a Direct3D 11 device to find its routines by (0x%08lX)", static_cast<unsigned long>(made));
    return false;
  }

  g_onPresent = onPresent;
  g_onClearDepth = onClearDepth;
  if (!hook::Install(present, reinterpret_cast<void*>(&OnPresent), reinterpret_cast<void**>(&g_present), "Direct3D's Present")) return false;
  if (onClearDepth) {
    hook::Install(clearDepth, reinterpret_cast<void*>(&OnClearDepth), reinterpret_cast<void**>(&g_clearDepth), "Direct3D's depth clear");
  }
  installed = true;
  return true;
}

ID3DBlob* Compile(const char* source, const char* entry, const char* target) {
  using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
  static CompileFn compile = nullptr;
  if (!compile) {
    HMODULE compiler = GetModuleHandleW(L"d3dcompiler_47.dll");
    if (!compiler) compiler = LoadLibraryW(L"d3dcompiler_47.dll");
    if (compiler) compile = reinterpret_cast<CompileFn>(GetProcAddress(compiler, "D3DCompile"));
    if (!compile) {
      log::Error("gfx: no shader compiler (d3dcompiler_47.dll)");
      return nullptr;
    }
  }
  ID3DBlob* code = nullptr;
  ID3DBlob* errors = nullptr;
  const HRESULT result = compile(source, std::strlen(source), nullptr, nullptr, nullptr, entry, target, 0, 0, &code, &errors);
  if (FAILED(result)) {
    log::Error("gfx: shader %s did not compile: %s", entry, errors ? static_cast<const char*>(errors->GetBufferPointer()) : "(no reason given)");
    if (code) code->Release();
    code = nullptr;
  }
  if (errors) errors->Release();
  return code;
}

SavedState::SavedState(ID3D11DeviceContext* context) : context_(context) {
  context_->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, targets_, &depth_);
  context_->RSGetViewports(&viewportCount_, viewports_);
  context_->IAGetInputLayout(&layout_);
  context_->IAGetPrimitiveTopology(&topology_);
  context_->VSGetShader(&vs_, nullptr, nullptr);
  context_->PSGetShader(&ps_, nullptr, nullptr);
  context_->GSGetShader(&gs_, nullptr, nullptr);
  context_->HSGetShader(&hs_, nullptr, nullptr);
  context_->DSGetShader(&ds_, nullptr, nullptr);
  context_->PSGetShaderResources(0, 2, resources_);
  context_->PSGetSamplers(0, 2, samplers_);
  context_->PSGetConstantBuffers(0, 1, &constants_);
  context_->OMGetBlendState(&blend_, blendFactor_, &sampleMask_);
  context_->OMGetDepthStencilState(&depthState_, &stencilRef_);
  context_->RSGetState(&raster_);
}

SavedState::~SavedState() {
  context_->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, targets_, depth_);
  context_->RSSetViewports(viewportCount_, viewports_);
  context_->IASetInputLayout(layout_);
  context_->IASetPrimitiveTopology(topology_);
  context_->VSSetShader(vs_, nullptr, 0);
  context_->PSSetShader(ps_, nullptr, 0);
  context_->GSSetShader(gs_, nullptr, 0);
  context_->HSSetShader(hs_, nullptr, 0);
  context_->DSSetShader(ds_, nullptr, 0);
  context_->PSSetShaderResources(0, 2, resources_);
  context_->PSSetSamplers(0, 2, samplers_);
  context_->PSSetConstantBuffers(0, 1, &constants_);
  context_->OMSetBlendState(blend_, blendFactor_, sampleMask_);
  context_->OMSetDepthStencilState(depthState_, stencilRef_);
  context_->RSSetState(raster_);
  for (auto* target : targets_) {
    if (target) target->Release();
  }
  if (depth_) depth_->Release();
  if (layout_) layout_->Release();
  if (vs_) vs_->Release();
  if (ps_) ps_->Release();
  if (gs_) gs_->Release();
  if (hs_) hs_->Release();
  if (ds_) ds_->Release();
  for (auto* resource : resources_) {
    if (resource) resource->Release();
  }
  for (auto* sampler : samplers_) {
    if (sampler) sampler->Release();
  }
  if (constants_) constants_->Release();
  if (blend_) blend_->Release();
  if (depthState_) depthState_->Release();
  if (raster_) raster_->Release();
}

}  // namespace csp::gfx
