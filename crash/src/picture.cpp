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
std::atomic<bool> g_on{true};
std::atomic<int> g_depthPick{0};
// The view the frame being drawn has, from the game's thread.
std::atomic_flag g_viewBusy = ATOMIC_FLAG_INIT;
View g_view{};
bool g_haveView = false;
// The camera's projection, the four numbers of it that matter here.
std::atomic<float> g_p0{0.0f}, g_p5{0.0f}, g_p10{0.0f}, g_p14{0.0f}, g_unrealPerUnit{0.5f};

// The picture is the back buffer and the scene's depth buffer put through
// this: colour as it is, and depth turned into a distance from the camera.
// The game's projection gives depth = -p22 + p32 / distance.
const char kPixelShader[] = R"(
Texture2D colour : register(t0);
Texture2D<float> depth : register(t1);
SamplerState nearest : register(s0);
cbuffer C : register(b0) {
  float4 part;       // of the back buffer the picture is in: u0, v0, u1, v1
  float4 depthPart;  // and of the depth buffer
  float p22, p32, unitsOut, haveDepth;
};
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
float4 ps(V i) : SV_Target {
  float2 uv = lerp(part.xy, part.zw, i.uv);
  float3 c = colour.SampleLevel(nearest, uv, 0).rgb;
  float d = depth.SampleLevel(nearest, lerp(depthPart.xy, depthPart.zw, i.uv), 0);
  float distance = haveDepth > 0.5 ? p32 / (d + p22) * unitsOut : 0.0;
  return float4(c, distance);
}
)";

struct Constants {
  float part[4];
  float depthPart[4];
  float p22, p32, unitsOut, haveDepth;
};

struct Sender {
  ComPtr<ID3D11Device> device;
  D3D11_TEXTURE2D_DESC back{};
  ComPtr<ID3D11Texture2D> colour;
  ComPtr<ID3D11ShaderResourceView> colourView;
  D3D11_TEXTURE2D_DESC depthWas{};
  ComPtr<ID3D11Texture2D> depthCopy;
  ComPtr<ID3D11ShaderResourceView> depthView;
  UINT width = 0, height = 0;  // of the picture sent
  ComPtr<ID3D11Texture2D> shared;
  ComPtr<ID3D11RenderTargetView> sharedTarget;
  HANDLE handle = nullptr;
  ComPtr<ID3D11VertexShader> vs;
  ComPtr<ID3D11PixelShader> ps;
  ComPtr<ID3D11SamplerState> sampler;
  ComPtr<ID3D11Buffer> constants;
  ComPtr<ID3D11DepthStencilState> noDepth;
  ComPtr<ID3D11RasterizerState> noCull;
  uint32_t frames = 0;
  bool broken = false;
} g;

// The depth buffer the scene is drawn with. The game need not draw its
// scene at the back buffer's size, so it is not known by that: it is taken
// to be the biggest depth buffer cleared in the course of a frame, as found
// in the frame before. If it is cleared a second time before the frame is
// shown (for something drawn over the scene), what it held is copied out
// first.
ComPtr<ID3D11Texture2D> g_sceneDepth;
int g_clears = 0;       // of it, this frame
bool g_copied = false;  // its contents are already in the copy
int g_logged = 0;
struct Cleared {
  ComPtr<ID3D11Texture2D> texture;
  D3D11_TEXTURE2D_DESC desc;
  int count;
};
Cleared g_cleared[8];
int g_clearedCount = 0;

DXGI_FORMAT Typeless(DXGI_FORMAT format) {
  switch (format) {
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS: return DXGI_FORMAT_R24G8_TYPELESS;
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_R32_FLOAT: return DXGI_FORMAT_R32_TYPELESS;
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_R16_UNORM: return DXGI_FORMAT_R16_TYPELESS;
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS: return DXGI_FORMAT_R32G8X24_TYPELESS;
    default: return DXGI_FORMAT_UNKNOWN;
  }
}

DXGI_FORMAT Readable(DXGI_FORMAT typeless) {
  switch (typeless) {
    case DXGI_FORMAT_R24G8_TYPELESS: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_R32G8X24_TYPELESS: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    default: return DXGI_FORMAT_UNKNOWN;
  }
}

DXGI_FORMAT Plain(DXGI_FORMAT format) {
  switch (format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
    default: return format;
  }
}

// A copy of the depth buffer that can be read by a shader, made to match it.
bool MatchDepthCopy(ID3D11Texture2D* depth) {
  D3D11_TEXTURE2D_DESC desc{};
  depth->GetDesc(&desc);
  if (g.depthCopy && desc.Width == g.depthWas.Width && desc.Height == g.depthWas.Height && desc.Format == g.depthWas.Format) return true;
  g.depthCopy.Reset();
  g.depthView.Reset();
  g.depthWas = desc;
  const DXGI_FORMAT typeless = Typeless(desc.Format);
  if (typeless == DXGI_FORMAT_UNKNOWN || desc.SampleDesc.Count != 1) {
    log::Error("picture: the scene's depth buffer is of a kind not handled (format %d, %u samples)", static_cast<int>(desc.Format), desc.SampleDesc.Count);
    return false;
  }
  D3D11_TEXTURE2D_DESC copy = desc;
  copy.Format = typeless;
  copy.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  copy.Usage = D3D11_USAGE_DEFAULT;
  copy.CPUAccessFlags = 0;
  copy.MiscFlags = 0;
  if (FAILED(g.device->CreateTexture2D(&copy, nullptr, &g.depthCopy))) return false;
  D3D11_SHADER_RESOURCE_VIEW_DESC view{};
  view.Format = Readable(typeless);
  view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
  view.Texture2D.MipLevels = 1;
  if (FAILED(g.device->CreateShaderResourceView(g.depthCopy.Get(), &view, &g.depthView))) {
    g.depthCopy.Reset();
    return false;
  }
  log::Info("picture: the scene's depth buffer is %ux%u, format %d", desc.Width, desc.Height, static_cast<int>(desc.Format));
  return true;
}

void OnClearDepth(ID3D11DeviceContext* context, ID3D11DepthStencilView* view, float) {
  if (!g_on.load(std::memory_order_relaxed) || !g.device || g.back.Width == 0) return;
  ComPtr<ID3D11Resource> resource;
  view->GetResource(&resource);
  ComPtr<ID3D11Texture2D> texture;
  if (!resource || FAILED(resource.As(&texture))) return;
  Cleared* entry = nullptr;
  for (int i = 0; i < g_clearedCount; ++i) {
    if (g_cleared[i].texture.Get() == texture.Get()) entry = &g_cleared[i];
  }
  if (!entry && g_clearedCount < 8) {
    entry = &g_cleared[g_clearedCount++];
    entry->texture = texture;
    texture->GetDesc(&entry->desc);
    entry->count = 0;
  }
  if (entry) ++entry->count;
  if (g_sceneDepth.Get() != texture.Get()) return;
  if (g_clears >= 1 && !g_copied && MatchDepthCopy(texture.Get())) {
    // Cleared a second time in one frame: keep what it held.
    context->CopyResource(g.depthCopy.Get(), texture.Get());
    g_copied = true;
  }
  ++g_clears;
}

bool Build(ID3D11Device* device, const D3D11_TEXTURE2D_DESC& back) {
  g = Sender{};
  g.device = device;
  g.back = back;

  D3D11_TEXTURE2D_DESC colour = back;
  colour.Format = Plain(back.Format);
  colour.SampleDesc = {1, 0};
  colour.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  colour.Usage = D3D11_USAGE_DEFAULT;
  colour.CPUAccessFlags = 0;
  colour.MiscFlags = 0;
  colour.MipLevels = 1;
  colour.ArraySize = 1;
  if (FAILED(device->CreateTexture2D(&colour, nullptr, &g.colour)) || FAILED(device->CreateShaderResourceView(g.colour.Get(), nullptr, &g.colourView))) return false;

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
  D3D11_BUFFER_DESC constants{};
  constants.ByteWidth = sizeof(Constants);
  constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  D3D11_DEPTH_STENCIL_DESC noDepth{};
  D3D11_RASTERIZER_DESC noCull{};
  noCull.FillMode = D3D11_FILL_SOLID;
  noCull.CullMode = D3D11_CULL_NONE;
  noCull.DepthClipEnable = TRUE;
  return SUCCEEDED(device->CreateSamplerState(&sampler, &g.sampler)) && SUCCEEDED(device->CreateBuffer(&constants, nullptr, &g.constants)) &&
         SUCCEEDED(device->CreateDepthStencilState(&noDepth, &g.noDepth)) && SUCCEEDED(device->CreateRasterizerState(&noCull, &g.noCull));
}

// The texture Spyro opens, the size of the picture.
bool MatchShared(UINT width, UINT height) {
  if (g.shared && width == g.width && height == g.height) return true;
  g.shared.Reset();
  g.sharedTarget.Reset();
  g.handle = nullptr;
  g.width = width;
  g.height = height;
  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = width;
  desc.Height = height;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
  desc.SampleDesc.Count = 1;
  desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
  ComPtr<IDXGIResource> resource;
  if (FAILED(g.device->CreateTexture2D(&desc, nullptr, &g.shared)) || FAILED(g.device->CreateRenderTargetView(g.shared.Get(), nullptr, &g.sharedTarget)) ||
      FAILED(g.shared.As(&resource)) || FAILED(resource->GetSharedHandle(&g.handle)) || !g.handle) {
    g.shared.Reset();
    return false;
  }
  log::Info("picture: sending %ux%u (the back buffer is %ux%u, format %d, %u samples)", width, height, g.back.Width, g.back.Height,
            static_cast<int>(g.back.Format), g.back.SampleDesc.Count);
  return true;
}

void Tell(const GuestPicture& picture) {
  if (g_link && g_link->block()) g_link->block()->picture.Write(picture);
}

void OnPresent(IDXGISwapChain* chain) {
  if (!g_on.load(std::memory_order_relaxed) || g.broken) {
    g_clears = 0;
    g_copied = false;
    return;
  }
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11Texture2D> back;
  if (FAILED(chain->GetDevice(IID_PPV_ARGS(&device))) || FAILED(chain->GetBuffer(0, IID_PPV_ARGS(&back)))) return;
  ComPtr<ID3D11DeviceContext> context;
  device->GetImmediateContext(&context);
  D3D11_TEXTURE2D_DESC desc{};
  back->GetDesc(&desc);
  if (device.Get() != g.device.Get() || desc.Width != g.back.Width || desc.Height != g.back.Height || desc.Format != g.back.Format) {
    g_sceneDepth.Reset();
    if (!Build(device.Get(), desc)) {
      log::Error("picture: could not make what is needed to send Crash's picture; none will be sent");
      g.broken = true;
      Tell(GuestPicture{});
      return;
    }
  }

  // Which part of the back buffer the scene is in: the game keeps its
  // picture's own shape and leaves bars above and below, or beside.
  const float p0 = g_p0.load(), p5 = g_p5.load(), p10 = g_p10.load(), p14 = g_p14.load();
  const float shape = p0 > 0.01f ? p5 / p0 : 16.0f / 9.0f, backShape = static_cast<float>(desc.Width) / static_cast<float>(desc.Height);
  Constants constants{{0.0f, 0.0f, 1.0f, 1.0f}, {0.0f, 0.0f, 1.0f, 1.0f}, p10, p14, g_unrealPerUnit.load(), 0.0f};
  UINT width = desc.Width, height = desc.Height;
  if (backShape < shape * 0.995f) {
    height = static_cast<UINT>(static_cast<float>(desc.Width) / shape + 0.5f);
    const float bar = (1.0f - static_cast<float>(height) / static_cast<float>(desc.Height)) * 0.5f;
    constants.part[1] = bar;
    constants.part[3] = 1.0f - bar;
  } else if (backShape > shape * 1.005f) {
    width = static_cast<UINT>(static_cast<float>(desc.Height) * shape + 0.5f);
    const float bar = (1.0f - static_cast<float>(width) / static_cast<float>(desc.Width)) * 0.5f;
    constants.part[0] = bar;
    constants.part[2] = 1.0f - bar;
  }
  if (width == 0 || height == 0 || !MatchShared(width, height)) return;

  if (desc.SampleDesc.Count > 1) context->ResolveSubresource(g.colour.Get(), 0, back.Get(), 0, Plain(desc.Format));
  else context->CopyResource(g.colour.Get(), back.Get());

  bool haveDepth = false;
  if (g_sceneDepth && p14 != 0.0f && MatchDepthCopy(g_sceneDepth.Get())) {
    if (!g_copied) context->CopyResource(g.depthCopy.Get(), g_sceneDepth.Get());
    haveDepth = true;
    // A depth buffer the size of the back buffer has the same bars as it;
    // one of another size is the scene and nothing else.
    if (g.depthWas.Width == desc.Width && g.depthWas.Height == desc.Height) std::memcpy(constants.depthPart, constants.part, sizeof(constants.part));
  }
  constants.haveDepth = haveDepth ? 1.0f : 0.0f;
  if (g_logged < 4 && g.frames % 120 == 60) {
    ++g_logged;
    char seen[400] = "";
    for (int i = 0; i < g_clearedCount; ++i) {
      char one[64];
      std::snprintf(one, sizeof(one), "%s%ux%u format %d x%d", i ? "; " : "", g_cleared[i].desc.Width, g_cleared[i].desc.Height,
                    static_cast<int>(g_cleared[i].desc.Format), g_cleared[i].count);
      std::strncat(seen, one, sizeof(seen) - std::strlen(seen) - 1);
    }
    log::Info("picture: frame %u: depth buffers cleared: %s. The scene's was cleared %d times, %s; projection %.4f %.4f %.4f %.4f", g.frames,
              g_clearedCount ? seen : "none", g_clears,
              !haveDepth ? "and there is no depth to send" : g_copied ? "and was copied out before the second" : "and was copied at the end",
              p0, p5, p10, p14);
  }
  // Of the depth buffers cleared in this frame that are the picture's size
  // (the others are shadows and the like), the one asked for, in the order
  // they were cleared, is taken for the scene's in the next frame.
  ID3D11Texture2D* chosen = nullptr;
  int ordinal = 0;
  const int wanted = g_depthPick.load(std::memory_order_relaxed);
  for (int i = 0; i < g_clearedCount; ++i) {
    const D3D11_TEXTURE2D_DESC& d = g_cleared[i].desc;
    if (!((d.Width == width && d.Height == height) || (d.Width == desc.Width && d.Height == desc.Height))) continue;
    if (ordinal++ == wanted || !chosen) chosen = g_cleared[i].texture.Get();
    if (ordinal > wanted) break;
  }
  if (chosen && chosen != g_sceneDepth.Get()) g_sceneDepth = chosen;
  for (int i = 0; i < g_clearedCount; ++i) g_cleared[i].texture.Reset();
  g_clearedCount = 0;

  {
    gfx::SavedState saved(context.Get());
    context->UpdateSubresource(g.constants.Get(), 0, nullptr, &constants, 0, 0);
    ID3D11RenderTargetView* target = g.sharedTarget.Get();
    context->OMSetRenderTargets(1, &target, nullptr);
    const D3D11_VIEWPORT viewport = {0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
    context->RSSetViewports(1, &viewport);
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(g.vs.Get(), nullptr, 0);
    context->PSSetShader(g.ps.Get(), nullptr, 0);
    context->GSSetShader(nullptr, nullptr, 0);
    context->HSSetShader(nullptr, nullptr, 0);
    context->DSSetShader(nullptr, nullptr, 0);
    ID3D11ShaderResourceView* views[2] = {g.colourView.Get(), haveDepth ? g.depthView.Get() : nullptr};
    context->PSSetShaderResources(0, 2, views);
    ID3D11SamplerState* sampler = g.sampler.Get();
    context->PSSetSamplers(0, 1, &sampler);
    ID3D11Buffer* buffer = g.constants.Get();
    context->PSSetConstantBuffers(0, 1, &buffer);
    context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    context->OMSetDepthStencilState(g.noDepth.Get(), 0);
    context->RSSetState(g.noCull.Get());
    context->Draw(3, 0);
  }
  // So that Spyro, opening the texture, finds this frame in it.
  context->Flush();

  GuestPicture picture{};
  picture.handle = reinterpret_cast<uint64_t>(g.handle);
  picture.width = width;
  picture.height = height;
  picture.frame = ++g.frames;
  picture.flags = haveDepth ? kPictureDepth : 0;
  picture.scale = constants.unitsOut;
  while (g_viewBusy.test_and_set(std::memory_order_acquire)) {
  }
  if (g_haveView) {
    picture.flags |= kPictureView;
    picture.cameraPosition = g_view.cameraPosition;
    picture.cameraRotation = g_view.cameraRotation;
    picture.cameraFov = g_view.cameraFov;
    picture.feet = g_view.feet;
  }
  g_viewBusy.clear(std::memory_order_release);
  // What the far end of the depth buffer comes out as, less a little.
  picture.farDistance = haveDepth ? p14 / (1.0f + p10) * constants.unitsOut * 0.98f : 0.0f;
  Tell(picture);
  g_clears = 0;
  g_copied = false;
}

}  // namespace

bool Install(SharedMapping* link) {
  g_link = link;
  return gfx::Install(&OnPresent, &OnClearDepth);
}

void SetCamera(const float projection[16], float unrealPerUnit) {
  g_p0.store(projection[0], std::memory_order_relaxed);
  g_p5.store(projection[5], std::memory_order_relaxed);
  g_p10.store(projection[10], std::memory_order_relaxed);
  g_p14.store(projection[14], std::memory_order_relaxed);
  g_unrealPerUnit.store(unrealPerUnit, std::memory_order_relaxed);
}

void SetView(const View* view) {
  while (g_viewBusy.test_and_set(std::memory_order_acquire)) {
  }
  g_haveView = view != nullptr;
  if (view) g_view = *view;
  g_viewBusy.clear(std::memory_order_release);
}

void PickDepth(int which) { g_depthPick.store(which, std::memory_order_relaxed); }

void Enable(bool on) {
  if (!on && g_on.exchange(false)) Tell(GuestPicture{});
  else g_on.store(on);
}

}  // namespace csp::picture
