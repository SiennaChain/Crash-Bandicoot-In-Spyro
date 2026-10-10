#include "picture.h"

#include <windows.h>

#include <wrl/client.h>

#include <atomic>
#include <cstdio>
#include <cstring>

#include "gfx.h"
#include "log.h"

using Microsoft::WRL::ComPtr;

namespace csp::picture {
namespace {

SharedMapping* g_link = nullptr;
std::atomic<bool> g_crash{false}, g_corner{false};
std::atomic<float> g_nearPlane{10.0f}, g_bias{2.0f};

// Crash's picture over Spyro's: wherever it says something of Crash's is
// (a distance short of "nothing here") and that is nearer than what Spyro
// drew there, its colour; elsewhere, nothing. Spyro's engine keeps depth as
// near plane / distance (1 at the near plane, 0 infinitely far).
const char kPixelShader[] = R"(
Texture2D picture : register(t0);
Texture2D<float> scene : register(t1);
SamplerState nearest : register(s0);
SamplerState smooth : register(s1);
cbuffer C : register(b0) {
  float farDistance, haveDepth, everything, haveScene;
  float2 scenePart;   // how much of Spyro's depth buffer the picture covers
  float nearPlane, bias;
};
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
float4 ps(V i) : SV_Target {
  float distance = picture.SampleLevel(nearest, i.uv, 0).a;
  if (everything < 0.5) {
    if (haveDepth < 0.5 || distance >= farDistance) discard;
    if (haveScene > 0.5) {
      float depth = scene.SampleLevel(nearest, i.uv * scenePart, 0);
      if (depth > 0.0 && distance > nearPlane / depth + bias) discard;
    }
  }
  return float4(picture.SampleLevel(smooth, i.uv, 0).rgb, 1);
}
)";

struct Constants {
  float farDistance, haveDepth, everything, haveScene;
  float scenePart[2];
  float nearPlane, bias;
};

// The depth buffer Spyro's scene was drawn with: of those cleared in the
// course of a frame, the first that is at least as big as the back buffer.
struct Cleared {
  ComPtr<ID3D11Texture2D> texture;
  D3D11_TEXTURE2D_DESC desc;
  float clearedTo;
  int count;
};
Cleared g_cleared[8];
int g_clearedCount = 0;
int g_logged = 0;

struct Receiver {
  ComPtr<ID3D11Device> device;
  uint64_t handle = 0;
  ComPtr<ID3D11Texture2D> picture;
  ComPtr<ID3D11ShaderResourceView> pictureView;
  ComPtr<ID3D11VertexShader> vs;
  ComPtr<ID3D11PixelShader> ps;
  ComPtr<ID3D11SamplerState> nearest, smooth;
  ComPtr<ID3D11Buffer> constants;
  ComPtr<ID3D11DepthStencilState> noDepth;
  ComPtr<ID3D11RasterizerState> noCull;
  bool broken = false;
  uint32_t lastFrame = 0, shown = 0;
  // Spyro's depth buffer, and a way to read it: straight from it if the
  // engine made it readable, from a copy if not.
  ID3D11Texture2D* sceneWas = nullptr;
  D3D11_TEXTURE2D_DESC sceneDesc{};
  ComPtr<ID3D11Texture2D> sceneCopy;
  ComPtr<ID3D11ShaderResourceView> sceneView;
} g;

void OnClearDepth(ID3D11DeviceContext*, ID3D11DepthStencilView* view, float clearedTo) {
  if (!g_crash.load(std::memory_order_relaxed)) return;
  ComPtr<ID3D11Resource> resource;
  view->GetResource(&resource);
  ComPtr<ID3D11Texture2D> texture;
  if (!resource || FAILED(resource.As(&texture))) return;
  for (int i = 0; i < g_clearedCount; ++i) {
    if (g_cleared[i].texture.Get() == texture.Get()) {
      ++g_cleared[i].count;
      return;
    }
  }
  if (g_clearedCount >= 8) return;
  Cleared& entry = g_cleared[g_clearedCount++];
  entry.texture = texture;
  texture->GetDesc(&entry.desc);
  entry.clearedTo = clearedTo;
  entry.count = 1;
}

DXGI_FORMAT ReadableDepth(DXGI_FORMAT format) {
  switch (format) {
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24G8_TYPELESS: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32G8X24_TYPELESS: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_R16_UNORM;
    default: return DXGI_FORMAT_UNKNOWN;
  }
}

DXGI_FORMAT TypelessDepth(DXGI_FORMAT format) {
  switch (ReadableDepth(format)) {
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS: return DXGI_FORMAT_R24G8_TYPELESS;
    case DXGI_FORMAT_R32_FLOAT: return DXGI_FORMAT_R32_TYPELESS;
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS: return DXGI_FORMAT_R32G8X24_TYPELESS;
    case DXGI_FORMAT_R16_UNORM: return DXGI_FORMAT_R16_TYPELESS;
    default: return DXGI_FORMAT_UNKNOWN;
  }
}

// Makes Spyro's depth buffer readable for this frame. False if it cannot be.
bool ReadScene(ID3D11DeviceContext* context, ID3D11Texture2D* scene) {
  D3D11_TEXTURE2D_DESC desc{};
  scene->GetDesc(&desc);
  const bool same = scene == g.sceneWas && desc.Width == g.sceneDesc.Width && desc.Height == g.sceneDesc.Height && desc.Format == g.sceneDesc.Format;
  if (!same) {
    g.sceneWas = scene;
    g.sceneDesc = desc;
    g.sceneCopy.Reset();
    g.sceneView.Reset();
    const DXGI_FORMAT readable = ReadableDepth(desc.Format);
    if (readable == DXGI_FORMAT_UNKNOWN || desc.SampleDesc.Count != 1) {
      log::Error("picture: Spyro's depth buffer is of a kind not handled (format %d, %u samples)", static_cast<int>(desc.Format), desc.SampleDesc.Count);
      return false;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = readable;
    view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    view.Texture2D.MipLevels = 1;
    const bool direct = (desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) && TypelessDepth(desc.Format) == desc.Format;
    if (direct) {
      if (FAILED(g.device->CreateShaderResourceView(scene, &view, &g.sceneView))) g.sceneView.Reset();
    }
    if (!g.sceneView) {
      D3D11_TEXTURE2D_DESC copy = desc;
      copy.Format = TypelessDepth(desc.Format);
      copy.BindFlags = D3D11_BIND_SHADER_RESOURCE;
      copy.Usage = D3D11_USAGE_DEFAULT;
      copy.CPUAccessFlags = 0;
      copy.MiscFlags = 0;
      if (FAILED(g.device->CreateTexture2D(&copy, nullptr, &g.sceneCopy)) || FAILED(g.device->CreateShaderResourceView(g.sceneCopy.Get(), &view, &g.sceneView))) {
        g.sceneCopy.Reset();
        g.sceneView.Reset();
      }
    }
    log::Info("picture: Spyro's depth buffer is %ux%u, format %d, read %s", desc.Width, desc.Height, static_cast<int>(desc.Format),
              !g.sceneView ? "NOT AT ALL" : g.sceneCopy ? "from a copy" : "directly");
  }
  if (!g.sceneView) return false;
  if (g.sceneCopy) context->CopyResource(g.sceneCopy.Get(), scene);
  return true;
}

bool Build(ID3D11Device* device) {
  g = Receiver{};
  g.device = device;
  ComPtr<ID3DBlob> vsCode, psCode;
  vsCode.Attach(gfx::Compile(gfx::kWholeTargetVertexShader, "vs", "vs_4_0"));
  psCode.Attach(gfx::Compile(kPixelShader, "ps", "ps_4_0"));
  if (!vsCode || !psCode) return false;
  if (FAILED(device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &g.vs)) ||
      FAILED(device->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &g.ps))) {
    return false;
  }
  D3D11_SAMPLER_DESC sampler{};
  sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
  sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler.MaxLOD = D3D11_FLOAT32_MAX;
  if (FAILED(device->CreateSamplerState(&sampler, &g.nearest))) return false;
  sampler.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
  if (FAILED(device->CreateSamplerState(&sampler, &g.smooth))) return false;
  D3D11_BUFFER_DESC constants{};
  constants.ByteWidth = sizeof(Constants);
  constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  D3D11_DEPTH_STENCIL_DESC noDepth{};
  D3D11_RASTERIZER_DESC noCull{};
  noCull.FillMode = D3D11_FILL_SOLID;
  noCull.CullMode = D3D11_CULL_NONE;
  noCull.DepthClipEnable = TRUE;
  return SUCCEEDED(device->CreateBuffer(&constants, nullptr, &g.constants)) && SUCCEEDED(device->CreateDepthStencilState(&noDepth, &g.noDepth)) &&
         SUCCEEDED(device->CreateRasterizerState(&noCull, &g.noCull));
}

void Draw(ID3D11DeviceContext* context, const D3D11_VIEWPORT& viewport, const Constants& constants) {
  context->UpdateSubresource(g.constants.Get(), 0, nullptr, &constants, 0, 0);
  context->RSSetViewports(1, &viewport);
  context->Draw(3, 0);
}

void OnPresent(IDXGISwapChain* chain) {
  const bool crash = g_crash.load(std::memory_order_relaxed), corner = g_corner.load(std::memory_order_relaxed);
  if ((!crash && !corner) || g.broken || !g_link || !g_link->block() || !g_link->PeerAlive()) return;
  GuestPicture picture{};
  if (!g_link->block()->picture.Read(picture) || picture.handle == 0 || picture.width == 0) return;

  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11Texture2D> back;
  if (FAILED(chain->GetDevice(IID_PPV_ARGS(&device))) || FAILED(chain->GetBuffer(0, IID_PPV_ARGS(&back)))) return;
  ComPtr<ID3D11DeviceContext> context;
  device->GetImmediateContext(&context);
  if (device.Get() != g.device.Get() && !Build(device.Get())) {
    log::Error("picture: could not make what is needed to draw Crash's picture; it will not be drawn");
    g.broken = true;
    return;
  }
  if (picture.handle != g.handle) {
    g.handle = picture.handle;
    g.picture.Reset();
    g.pictureView.Reset();
    const HRESULT opened = device->OpenSharedResource(reinterpret_cast<HANDLE>(picture.handle), IID_PPV_ARGS(&g.picture));
    if (FAILED(opened) || FAILED(device->CreateShaderResourceView(g.picture.Get(), nullptr, &g.pictureView))) {
      log::Error("picture: Crash's picture could not be opened (0x%08lX)", static_cast<unsigned long>(opened));
      g.picture.Reset();
      return;
    }
    D3D11_TEXTURE2D_DESC desc{};
    back->GetDesc(&desc);
    log::Info("picture: Crash's picture is open, %ux%u, %s; Spyro's back buffer is %ux%u, format %d", picture.width, picture.height,
              (picture.flags & kPictureDepth) ? "with distances" : "WITHOUT distances", desc.Width, desc.Height, static_cast<int>(desc.Format));
  }
  if (!g.pictureView) return;
  ComPtr<ID3D11RenderTargetView> target;
  if (FAILED(device->CreateRenderTargetView(back.Get(), nullptr, &target))) return;
  D3D11_TEXTURE2D_DESC desc{};
  back->GetDesc(&desc);

  // Spyro's own depth, to hide Crash behind what is nearer than he is.
  ID3D11Texture2D* scene = nullptr;
  for (int i = 0; i < g_clearedCount && !scene; ++i) {
    if (g_cleared[i].desc.Width >= desc.Width && g_cleared[i].desc.Height >= desc.Height && g_cleared[i].clearedTo == 0.0f) scene = g_cleared[i].texture.Get();
  }
  if (g_logged < 3 && crash && (g.shown % 240) == 120) {
    ++g_logged;
    char seen[500] = "";
    for (int i = 0; i < g_clearedCount; ++i) {
      char one[80];
      std::snprintf(one, sizeof(one), "%s%ux%u format %d to %.0f x%d", i ? "; " : "", g_cleared[i].desc.Width, g_cleared[i].desc.Height,
                    static_cast<int>(g_cleared[i].desc.Format), g_cleared[i].clearedTo, g_cleared[i].count);
      std::strncat(seen, one, sizeof(seen) - std::strlen(seen) - 1);
    }
    log::Info("picture: depth buffers Spyro cleared this frame: %s", g_clearedCount ? seen : "none");
  }
  const bool haveScene = crash && scene && ReadScene(context.Get(), scene);
  const float scenePartX = haveScene ? static_cast<float>(desc.Width) / static_cast<float>(g.sceneDesc.Width) : 1.0f;
  const float scenePartY = haveScene ? static_cast<float>(desc.Height) / static_cast<float>(g.sceneDesc.Height) : 1.0f;
  for (int i = 0; i < g_clearedCount; ++i) g_cleared[i].texture.Reset();
  g_clearedCount = 0;

  {
    gfx::SavedState saved(context.Get());
    ID3D11RenderTargetView* targets = target.Get();
    context->OMSetRenderTargets(1, &targets, nullptr);
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(g.vs.Get(), nullptr, 0);
    context->PSSetShader(g.ps.Get(), nullptr, 0);
    context->GSSetShader(nullptr, nullptr, 0);
    context->HSSetShader(nullptr, nullptr, 0);
    context->DSSetShader(nullptr, nullptr, 0);
    ID3D11ShaderResourceView* views[2] = {g.pictureView.Get(), haveScene ? g.sceneView.Get() : nullptr};
    context->PSSetShaderResources(0, 2, views);
    ID3D11SamplerState* samplers[2] = {g.nearest.Get(), g.smooth.Get()};
    context->PSSetSamplers(0, 2, samplers);
    ID3D11Buffer* buffer = g.constants.Get();
    context->PSSetConstantBuffers(0, 1, &buffer);
    context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    context->OMSetDepthStencilState(g.noDepth.Get(), 0);
    context->RSSetState(g.noCull.Get());
    const float haveDepth = (picture.flags & kPictureDepth) ? 1.0f : 0.0f;
    const float width = static_cast<float>(desc.Width), height = static_cast<float>(desc.Height);
    const float nearPlane = g_nearPlane.load(std::memory_order_relaxed), bias = g_bias.load(std::memory_order_relaxed);
    if (crash) {
      Draw(context.Get(), {0.0f, 0.0f, width, height, 0.0f, 1.0f},
           {picture.farDistance, haveDepth, 0.0f, haveScene ? 1.0f : 0.0f, {scenePartX, scenePartY}, nearPlane, bias});
    }
    if (corner) {
      Draw(context.Get(), {width * 0.72f, height * 0.04f, width * 0.26f, height * 0.26f, 0.0f, 1.0f},
           {picture.farDistance, haveDepth, 1.0f, 0.0f, {1.0f, 1.0f}, nearPlane, bias});
    }
  }
  if (picture.frame != g.lastFrame) {
    g.lastFrame = picture.frame;
    ++g.shown;
  }
}

}  // namespace

bool Install(SharedMapping* link) {
  g_link = link;
  return gfx::Install(&OnPresent, &OnClearDepth);
}

void Depth(float nearPlane, float bias) {
  g_nearPlane.store(nearPlane, std::memory_order_relaxed);
  g_bias.store(bias, std::memory_order_relaxed);
}

void Show(bool crash, bool corner) {
  g_crash.store(crash, std::memory_order_relaxed);
  g_corner.store(corner, std::memory_order_relaxed);
}

}  // namespace csp::picture
