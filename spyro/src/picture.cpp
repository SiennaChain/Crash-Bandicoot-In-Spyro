#include "picture.h"

#include <windows.h>

#include <wrl/client.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "gfx.h"
#include "log.h"

using Microsoft::WRL::ComPtr;

namespace csp::picture {
namespace {

SharedMapping* g_link = nullptr;
CameraFn g_camera = nullptr;
std::atomic<bool> g_crash{false}, g_corner{false};
std::atomic<float> g_nearPlane{5.0f}, g_bias{2.0f}, g_shadow{0.45f};
std::atomic<int> g_steady{1};

// Crash's picture over Spyro's: wherever it says something of Crash's is
// (a distance short of "nothing here") and that is nearer than what Spyro
// drew there, its colour; elsewhere, nothing. Spyro's engine keeps depth as
// near plane / distance (1 at the near plane, 0 infinitely far).
//
// The picture was drawn a frame or two ago, through the camera Spyro had
// then. `warp` moves and sizes it so that Crash is where the camera of the
// frame now being shown would see him: for a point of this frame, where in
// the picture to look.
const char kPictureShader[] = R"(
Texture2D picture : register(t0);
Texture2D<float> scene : register(t1);
SamplerState nearest : register(s0);
SamplerState smooth : register(s1);
cbuffer C : register(b0) {
  float farDistance, haveDepth, everything, haveScene;
  float2 scenePart;   // how much of Spyro's depth buffer the frame covers
  float nearPlane, bias;
  float4 warp;        // picture point = warp.xy + warp.zw * frame point, both -1..1
  float distanceScale;
  float3 unused;
};
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
float4 ps(V i) : SV_Target {
  float2 at = warp.xy + warp.zw * float2(i.uv.x * 2 - 1, 1 - i.uv.y * 2);
  float2 uv = float2(at.x * 0.5 + 0.5, 0.5 - at.y * 0.5);
  if (everything < 0.5) {
    if (uv.x < 0 || uv.y < 0 || uv.x > 1 || uv.y > 1 || haveDepth < 0.5) discard;
    float distance = picture.SampleLevel(nearest, uv, 0).a;
    if (distance >= farDistance) discard;
    if (haveScene > 0.5) {
      float depth = scene.SampleLevel(nearest, i.uv * scenePart, 0);
      if (depth > 0.0 && distance * distanceScale > nearPlane / depth + bias) discard;
    }
  }
  return float4(picture.SampleLevel(smooth, uv, 0).rgb, 1);
}
)";

// His shadow: Spyro's own picture made darker wherever what Spyro drew is
// under Crash's feet, found by working each point's place in the world out
// from its depth. Multiplied into the frame before Crash is drawn.
const char kShadowShader[] = R"(
Texture2D<float> scene : register(t1);
SamplerState nearest : register(s0);
cbuffer S : register(b0) {
  float3 eye; float tanAcross;
  float3 ahead; float tanUp;
  float3 right; float nearPlane;
  float3 up; float radius;
  float3 feet; float strength;
  float2 scenePart; float2 unused;
};
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
float4 ps(V i) : SV_Target {
  float depth = scene.SampleLevel(nearest, i.uv * scenePart, 0);
  if (depth <= 0.0) discard;
  float distance = nearPlane / depth;
  float2 at = float2(i.uv.x * 2 - 1, 1 - i.uv.y * 2);
  float3 world = eye + distance * (ahead + at.x * tanAcross * right + at.y * tanUp * up);
  float away = length(world.xy - feet.xy) / radius;
  float below = feet.z - world.z;
  if (away >= 1.0 || below < -0.4 * radius || below > 12.0 * radius) discard;
  float shade = strength * (1.0 - away * away) * saturate(1.0 - below / (12.0 * radius));
  return float4(1 - shade, 1 - shade, 1 - shade, 1);
}
)";

struct PictureConstants {
  float farDistance, haveDepth, everything, haveScene;
  float scenePart[2];
  float nearPlane, bias;
  float warp[4];
  float distanceScale;
  float unused[3];
};

struct ShadowConstants {
  float eye[3], tanAcross;
  float ahead[3], tanUp;
  float right[3], nearPlane;
  float up[3], radius;
  float feet[3], strength;
  float scenePart[2], unused[2];
};

// The depth buffer Spyro's scene was drawn with: of those cleared in the
// course of a frame, the first that is at least as big as the back buffer
// and cleared to 0.
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
  ComPtr<ID3D11PixelShader> picturePs, shadowPs;
  ComPtr<ID3D11SamplerState> nearest, smooth;
  ComPtr<ID3D11Buffer> pictureConstants, shadowConstants;
  ComPtr<ID3D11DepthStencilState> noDepth;
  ComPtr<ID3D11RasterizerState> noCull;
  ComPtr<ID3D11BlendState> multiply;
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
  ComPtr<ID3DBlob> vsCode, pictureCode, shadowCode;
  vsCode.Attach(gfx::Compile(gfx::kWholeTargetVertexShader, "vs", "vs_4_0"));
  pictureCode.Attach(gfx::Compile(kPictureShader, "ps", "ps_4_0"));
  shadowCode.Attach(gfx::Compile(kShadowShader, "ps", "ps_4_0"));
  if (!vsCode || !pictureCode || !shadowCode) return false;
  if (FAILED(device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &g.vs)) ||
      FAILED(device->CreatePixelShader(pictureCode->GetBufferPointer(), pictureCode->GetBufferSize(), nullptr, &g.picturePs)) ||
      FAILED(device->CreatePixelShader(shadowCode->GetBufferPointer(), shadowCode->GetBufferSize(), nullptr, &g.shadowPs))) {
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
  constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  constants.ByteWidth = sizeof(PictureConstants);
  if (FAILED(device->CreateBuffer(&constants, nullptr, &g.pictureConstants))) return false;
  constants.ByteWidth = sizeof(ShadowConstants);
  if (FAILED(device->CreateBuffer(&constants, nullptr, &g.shadowConstants))) return false;
  D3D11_DEPTH_STENCIL_DESC noDepth{};
  D3D11_RASTERIZER_DESC noCull{};
  noCull.FillMode = D3D11_FILL_SOLID;
  noCull.CullMode = D3D11_CULL_NONE;
  noCull.DepthClipEnable = TRUE;
  // What is drawn times what is already there.
  D3D11_BLEND_DESC multiply{};
  multiply.RenderTarget[0].BlendEnable = TRUE;
  multiply.RenderTarget[0].SrcBlend = D3D11_BLEND_ZERO;
  multiply.RenderTarget[0].DestBlend = D3D11_BLEND_SRC_COLOR;
  multiply.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
  multiply.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
  multiply.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
  multiply.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
  multiply.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  return SUCCEEDED(device->CreateDepthStencilState(&noDepth, &g.noDepth)) && SUCCEEDED(device->CreateRasterizerState(&noCull, &g.noCull)) &&
         SUCCEEDED(device->CreateBlendState(&multiply, &g.multiply));
}

// A camera of Spyro's as three directions in its world: Unreal's pitch up,
// yaw from x towards y, no roll.
struct Eye {
  float at[3], ahead[3], right[3], up[3], tanAcross;
};

Eye MakeEye(const Vec3& position, const Vec3& rotation, float fov) {
  const float pitch = rotation.x * 3.14159265f / 180.0f, yaw = rotation.y * 3.14159265f / 180.0f;
  const float cp = std::cos(pitch), sp = std::sin(pitch), cy = std::cos(yaw), sy = std::sin(yaw);
  return {{position.x, position.y, position.z}, {cp * cy, cp * sy, sp}, {-sy, cy, 0.0f}, {-sp * cy, -sp * sy, cp}, std::tan(fov * 3.14159265f / 360.0f)};
}

// Where a point of the world is on that camera's frame (-1..1 each way) and
// how far ahead. False if it is not in front.
bool Project(const Eye& eye, const float point[3], float shape, float& x, float& y, float& ahead) {
  const float d[3] = {point[0] - eye.at[0], point[1] - eye.at[1], point[2] - eye.at[2]};
  const auto dot = [&](const float a[3]) { return d[0] * a[0] + d[1] * a[1] + d[2] * a[2]; };
  ahead = dot(eye.ahead);
  if (ahead < 1.0f) return false;
  x = dot(eye.right) / (ahead * eye.tanAcross);
  y = dot(eye.up) / (ahead * eye.tanAcross / shape);
  return true;
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
  D3D11_TEXTURE2D_DESC desc{};
  back->GetDesc(&desc);
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
    log::Info("picture: Crash's picture is open, %ux%u; Spyro's back buffer is %ux%u, format %d", picture.width, picture.height, desc.Width,
              desc.Height, static_cast<int>(desc.Format));
  }
  if (!g.pictureView) return;
  ComPtr<ID3D11RenderTargetView> target;
  if (FAILED(device->CreateRenderTargetView(back.Get(), nullptr, &target))) return;

  // Spyro's own depth, to hide Crash behind what is nearer than he is.
  ID3D11Texture2D* scene = nullptr;
  for (int i = 0; i < g_clearedCount && !scene; ++i) {
    if (g_cleared[i].desc.Width >= desc.Width && g_cleared[i].desc.Height >= desc.Height && g_cleared[i].clearedTo == 0.0f) scene = g_cleared[i].texture.Get();
  }
  if (g_logged < 2 && crash && (g.shown % 240) == 120) {
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
  const float scenePart[2] = {haveScene ? static_cast<float>(desc.Width) / static_cast<float>(g.sceneDesc.Width) : 1.0f,
                              haveScene ? static_cast<float>(desc.Height) / static_cast<float>(g.sceneDesc.Height) : 1.0f};
  for (int i = 0; i < g_clearedCount; ++i) g_cleared[i].texture.Reset();
  g_clearedCount = 0;

  // The camera this frame of Spyro's was drawn through, as near as can be
  // told: the engine's, read now. The game is at most a frame ahead of what
  // is being shown and moves its camera late in its frame.
  const float width = static_cast<float>(desc.Width), height = static_cast<float>(desc.Height), shape = width / height;
  const float nearPlane = g_nearPlane.load(std::memory_order_relaxed), bias = g_bias.load(std::memory_order_relaxed);
  PictureConstants constants{picture.farDistance, (picture.flags & kPictureDepth) ? 1.0f : 0.0f, 0.0f, haveScene ? 1.0f : 0.0f,
                             {scenePart[0], scenePart[1]}, nearPlane, bias, {0.0f, 0.0f, 1.0f, 1.0f}, 1.0f, {}};
  Vec3 nowPosition{}, nowRotation{};
  float nowFov = 0.0f;
  const bool haveView = crash && (picture.flags & kPictureView) && g_camera && g_camera(nowPosition, nowRotation, nowFov);
  Eye now{};
  if (haveView) {
    now = MakeEye(nowPosition, nowRotation, nowFov);
    if (g_steady.load(std::memory_order_relaxed)) {
      // His middle, as the picture's camera saw it and as this frame's does.
      const Eye then = MakeEye(picture.cameraPosition, picture.cameraRotation, picture.cameraFov);
      const float middle[3] = {picture.feet.x, picture.feet.y, picture.feet.z + 100.0f * picture.scale};
      float thenX, thenY, thenAhead, nowX, nowY, nowAhead;
      if (Project(then, middle, shape, thenX, thenY, thenAhead) && Project(now, middle, shape, nowX, nowY, nowAhead)) {
        const float bigger = thenAhead / nowAhead;  // nearer now: drawn bigger
        if (bigger > 0.5f && bigger < 2.0f && std::fabs(nowX - thenX) < 0.6f && std::fabs(nowY - thenY) < 0.6f) {
          constants.warp[0] = thenX - nowX / bigger;
          constants.warp[1] = thenY - nowY / bigger;
          constants.warp[2] = constants.warp[3] = 1.0f / bigger;
          constants.distanceScale = 1.0f / bigger;
        }
      }
    }
  }

  {
    gfx::SavedState saved(context.Get());
    ID3D11RenderTargetView* targets = target.Get();
    context->OMSetRenderTargets(1, &targets, nullptr);
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(g.vs.Get(), nullptr, 0);
    context->GSSetShader(nullptr, nullptr, 0);
    context->HSSetShader(nullptr, nullptr, 0);
    context->DSSetShader(nullptr, nullptr, 0);
    ID3D11ShaderResourceView* views[2] = {g.pictureView.Get(), haveScene ? g.sceneView.Get() : nullptr};
    context->PSSetShaderResources(0, 2, views);
    ID3D11SamplerState* samplers[2] = {g.nearest.Get(), g.smooth.Get()};
    context->PSSetSamplers(0, 2, samplers);
    context->OMSetDepthStencilState(g.noDepth.Get(), 0);
    context->RSSetState(g.noCull.Get());
    const D3D11_VIEWPORT whole = {0.0f, 0.0f, width, height, 0.0f, 1.0f};
    context->RSSetViewports(1, &whole);

    const float strength = g_shadow.load(std::memory_order_relaxed);
    if (crash && haveView && haveScene && strength > 0.0f) {
      ShadowConstants shadow{};
      std::memcpy(shadow.eye, now.at, sizeof(shadow.eye));
      std::memcpy(shadow.ahead, now.ahead, sizeof(shadow.ahead));
      std::memcpy(shadow.right, now.right, sizeof(shadow.right));
      std::memcpy(shadow.up, now.up, sizeof(shadow.up));
      shadow.tanAcross = now.tanAcross;
      shadow.tanUp = now.tanAcross / shape;
      shadow.nearPlane = nearPlane;
      shadow.radius = 55.0f * picture.scale;
      shadow.feet[0] = picture.feet.x;
      shadow.feet[1] = picture.feet.y;
      shadow.feet[2] = picture.feet.z;
      shadow.strength = strength;
      shadow.scenePart[0] = scenePart[0];
      shadow.scenePart[1] = scenePart[1];
      context->UpdateSubresource(g.shadowConstants.Get(), 0, nullptr, &shadow, 0, 0);
      ID3D11Buffer* buffer = g.shadowConstants.Get();
      context->PSSetConstantBuffers(0, 1, &buffer);
      context->PSSetShader(g.shadowPs.Get(), nullptr, 0);
      context->OMSetBlendState(g.multiply.Get(), nullptr, 0xFFFFFFFF);
      context->Draw(3, 0);
    }

    ID3D11Buffer* buffer = g.pictureConstants.Get();
    context->PSSetConstantBuffers(0, 1, &buffer);
    context->PSSetShader(g.picturePs.Get(), nullptr, 0);
    context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    if (crash) {
      context->UpdateSubresource(g.pictureConstants.Get(), 0, nullptr, &constants, 0, 0);
      context->Draw(3, 0);
    }
    if (corner) {
      const PictureConstants all{picture.farDistance, 0.0f, 1.0f, 0.0f, {1.0f, 1.0f}, nearPlane, bias, {0.0f, 0.0f, 1.0f, 1.0f}, 1.0f, {}};
      const D3D11_VIEWPORT inset = {width * 0.72f, height * 0.04f, width * 0.26f, height * 0.26f, 0.0f, 1.0f};
      context->UpdateSubresource(g.pictureConstants.Get(), 0, nullptr, &all, 0, 0);
      context->RSSetViewports(1, &inset);
      context->Draw(3, 0);
    }
  }
  if (picture.frame != g.lastFrame) {
    g.lastFrame = picture.frame;
    ++g.shown;
  }
}

}  // namespace

bool Install(SharedMapping* link, CameraFn camera) {
  g_link = link;
  g_camera = camera;
  return gfx::Install(&OnPresent, &OnClearDepth);
}

void Show(bool crash, bool corner) {
  g_crash.store(crash, std::memory_order_relaxed);
  g_corner.store(corner, std::memory_order_relaxed);
}

void Depth(float nearPlane, float bias) {
  g_nearPlane.store(nearPlane, std::memory_order_relaxed);
  g_bias.store(bias, std::memory_order_relaxed);
}

void Looks(float shadow, bool steady) {
  g_shadow.store(shadow, std::memory_order_relaxed);
  g_steady.store(steady ? 1 : 0, std::memory_order_relaxed);
}

}  // namespace csp::picture
