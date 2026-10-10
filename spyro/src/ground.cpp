#include "ground.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

// PhysX 3.4's own headers, as shipped with the engine the game was built
// on (CMakeLists.txt says where they are looked for). Only interfaces are
// used, so nothing of PhysX is linked: the game's own DLL is asked.
#include "PxPhysics.h"
#include "PxQueryFiltering.h"
#include "PxQueryReport.h"
#include "PxRigidActor.h"
#include "PxScene.h"
#include "PxShape.h"
#include "foundation/PxTransform.h"
#include "geometry/PxBoxGeometry.h"
#include "geometry/PxCapsuleGeometry.h"
#include "geometry/PxConvexMesh.h"
#include "geometry/PxConvexMeshGeometry.h"
#include "geometry/PxSphereGeometry.h"
#include "geometry/PxTriangleMesh.h"
#include "geometry/PxTriangleMeshGeometry.h"

namespace ground {
namespace {

using namespace physx;

// Unreal's use of a shape's query filter data (Engine/Public/Physics/
// PhysicsFiltering.h, 4.19): word1 is a bit for each channel the shape
// blocks; word3 has the shape's own channel in five bits below the top six,
// and flags in the low bits.
constexpr uint32_t kSimpleCollision = 0x1, kComplexCollision = 0x2;
constexpr uint32_t kChannelPawn = 2;
uint32_t ChannelOf(uint32_t word3) { return (word3 << 6) >> 27; }

PxPhysics* Physics() {
  static PxPhysics* (*get)() = nullptr;
  if (!get) {
    const HMODULE physx3 = GetModuleHandleW(L"PhysX3_x64.dll");
    if (!physx3) return nullptr;
    // "PxPhysics& PxGetPhysics()": a reference comes back as an address.
    get = reinterpret_cast<PxPhysics* (*)()>(GetProcAddress(physx3, "PxGetPhysics"));
    if (!get) return nullptr;
  }
  return get();
}

// Every shape the box touches, however many: PhysX hands them over a
// bufferful at a time.
struct Touches : PxOverlapCallback {
  PxOverlapHit buffer[256];
  std::vector<PxOverlapHit> all;
  Touches() : PxOverlapCallback(buffer, 256) {}
  PxAgain processTouches(const PxOverlapHit* hits, PxU32 count) override {
    all.insert(all.end(), hits, hits + count);
    return true;
  }
};

struct Sink {
  std::vector<float>& out;
  PxVec3 centre, low, high;  // the box, in the world
  uint32_t room;
  bool full = false;

  // One triangle, in the world. Kept if its own box reaches into ours.
  void Add(const PxVec3& a, const PxVec3& b, const PxVec3& c) {
    const PxVec3 lo = a.minimum(b).minimum(c), hi = a.maximum(b).maximum(c);
    if (hi.x < low.x || hi.y < low.y || hi.z < low.z || lo.x > high.x || lo.y > high.y || lo.z > high.z) return;
    if (room == 0) {
      full = true;
      return;
    }
    --room;
    const PxVec3 corners[3] = {a - centre, b - centre, c - centre};
    for (const PxVec3& p : corners) {
      out.push_back(p.x);
      out.push_back(p.y);
      out.push_back(p.z);
    }
  }
};

void AddMesh(Sink& sink, const PxShape& shape, const PxTransform& pose, std::vector<PxVec3>& scratch) {
  PxTriangleMeshGeometry g;
  if (!shape.getTriangleMeshGeometry(g) || !g.triangleMesh) return;
  const PxTriangleMesh& mesh = *g.triangleMesh;
  const PxMat33 scale = g.scale.toMat33();
  const bool flipped = g.scale.hasNegativeDeterminant();
  const PxVec3* vertices = mesh.getVertices();
  const PxU32 vertexCount = mesh.getNbVertices(), count = mesh.getNbTriangles();
  scratch.resize(vertexCount);
  for (PxU32 i = 0; i < vertexCount; ++i) scratch[i] = pose.transform(scale * vertices[i]);
  const bool small = mesh.getTriangleMeshFlags() & PxTriangleMeshFlag::e16_BIT_INDICES;
  const void* indices = mesh.getTriangles();
  for (PxU32 t = 0; t < count && !sink.full; ++t) {
    PxU32 i0, i1, i2;
    if (small) {
      const PxU16* at = static_cast<const PxU16*>(indices) + 3 * t;
      i0 = at[0], i1 = at[1], i2 = at[2];
    } else {
      const PxU32* at = static_cast<const PxU32*>(indices) + 3 * t;
      i0 = at[0], i1 = at[1], i2 = at[2];
    }
    if (i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount) continue;
    if (flipped) std::swap(i1, i2);
    sink.Add(scratch[i0], scratch[i1], scratch[i2]);
  }
}

void AddConvex(Sink& sink, const PxShape& shape, const PxTransform& pose, std::vector<PxVec3>& scratch) {
  PxConvexMeshGeometry g;
  if (!shape.getConvexMeshGeometry(g) || !g.convexMesh) return;
  const PxConvexMesh& mesh = *g.convexMesh;
  const PxMat33 scale = g.scale.toMat33();
  const bool flipped = g.scale.hasNegativeDeterminant();
  const PxVec3* vertices = mesh.getVertices();
  const PxU32 vertexCount = mesh.getNbVertices();
  scratch.resize(vertexCount);
  for (PxU32 i = 0; i < vertexCount; ++i) scratch[i] = pose.transform(scale * vertices[i]);
  const PxU8* indices = mesh.getIndexBuffer();
  for (PxU32 p = 0, polygons = mesh.getNbPolygons(); p < polygons && !sink.full; ++p) {
    PxHullPolygon polygon;
    if (!mesh.getPolygonData(p, polygon)) continue;
    const PxU8* at = indices + polygon.mIndexBase;
    // A fan from its first corner.
    for (PxU32 k = 1; k + 1 < polygon.mNbVerts; ++k) {
      PxU32 i0 = at[0], i1 = at[k], i2 = at[k + 1];
      if (i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount) continue;
      if (flipped) std::swap(i1, i2);
      sink.Add(scratch[i0], scratch[i1], scratch[i2]);
    }
  }
}

void AddBox(Sink& sink, const PxShape& shape, const PxTransform& pose) {
  PxBoxGeometry g;
  if (!shape.getBoxGeometry(g)) return;
  PxVec3 corner[8];
  for (int i = 0; i < 8; ++i) {
    corner[i] = pose.transform(PxVec3((i & 1) ? g.halfExtents.x : -g.halfExtents.x, (i & 2) ? g.halfExtents.y : -g.halfExtents.y,
                                      (i & 4) ? g.halfExtents.z : -g.halfExtents.z));
  }
  // Each side as two triangles, anticlockwise seen from outside.
  static const int kSides[6][4] = {{0, 4, 6, 2}, {1, 3, 7, 5}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 2, 3, 1}, {4, 5, 7, 6}};
  for (const auto& side : kSides) {
    sink.Add(corner[side[0]], corner[side[1]], corner[side[2]]);
    sink.Add(corner[side[0]], corner[side[2]], corner[side[3]]);
  }
}

// A ball, or a ball pulled apart along its own x by `halfHeight` each way,
// which is how PhysX lays a capsule. Coarse: it is ground, not a picture.
void AddRound(Sink& sink, const PxTransform& pose, float radius, float halfHeight) {
  constexpr int kAround = 10, kRings = 6;  // rings: pole, 45, equator, equator, 45, pole
  static const float kLatitude[kRings] = {-1.5708f, -0.7854f, 0.0f, 0.0f, 0.7854f, 1.5708f};
  PxVec3 point[kRings][kAround];
  for (int r = 0; r < kRings; ++r) {
    const float along = radius * std::sin(kLatitude[r]) + (r < kRings / 2 ? -halfHeight : halfHeight);
    const float out = radius * std::cos(kLatitude[r]);
    for (int a = 0; a < kAround; ++a) {
      const float turn = 6.2831853f * static_cast<float>(a) / kAround;
      point[r][a] = pose.transform(PxVec3(along, out * std::cos(turn), out * std::sin(turn)));
    }
  }
  for (int r = 0; r + 1 < kRings; ++r) {
    if (r == kRings / 2 - 1 && halfHeight <= 0.0f) continue;  // a ball has no middle
    for (int a = 0; a < kAround; ++a) {
      const int b = (a + 1) % kAround;
      if (r > 0) sink.Add(point[r][a], point[r][b], point[r + 1][b]);
      if (r + 2 < kRings) sink.Add(point[r][a], point[r + 1][b], point[r + 1][a]);
    }
  }
}

const char* KindName(PxGeometryType::Enum type) {
  switch (type) {
    case PxGeometryType::eSPHERE: return "ball";
    case PxGeometryType::ePLANE: return "plane";
    case PxGeometryType::eCAPSULE: return "capsule";
    case PxGeometryType::eBOX: return "box";
    case PxGeometryType::eCONVEXMESH: return "convex";
    case PxGeometryType::eTRIANGLEMESH: return "mesh";
    case PxGeometryType::eHEIGHTFIELD: return "heightfield";
    default: return "other";
  }
}

}  // namespace

bool Gather(const csp::Vec3& centre, const Options& options, std::vector<float>& out, Stats& stats) {
  PxPhysics* physics = Physics();
  if (!physics) return false;
  PxScene* scenes[8] = {};
  const PxU32 sceneCount = physics->getScenes(scenes, 8);
  stats.scenes = static_cast<int>(sceneCount);

  const float middle = (options.up - options.down) * 0.5f, tall = (options.up + options.down) * 0.5f;
  const PxVec3 c(centre.x, centre.y, centre.z);
  Sink sink{out, c, c - PxVec3(options.reach, options.reach, options.down), c + PxVec3(options.reach, options.reach, options.up),
            options.maxTriangles};
  const PxBoxGeometry box(options.reach, options.reach, tall);
  const PxTransform boxPose(PxVec3(centre.x, centre.y, centre.z + middle));
  // No filter data: everything the scene can be asked about answers, and
  // the choosing is done below, where it can be counted.
  PxQueryFilterData everything;
  everything.flags = PxQueryFlag::eSTATIC | PxQueryFlag::eDYNAMIC | PxQueryFlag::eNO_BLOCK;

  std::map<uint64_t, int> kinds;  // (kind, own channel, simple, complex, blocks) -> how many
  std::vector<PxVec3> scratch;
  const uint32_t before = static_cast<uint32_t>(out.size() / 9);
  for (PxU32 s = 0; s < sceneCount; ++s) {
    PxScene* scene = scenes[s];
    if (!scene) continue;
    scene->lockRead(__FILE__, __LINE__);
    Touches touches;
    scene->overlap(box, boxPose, touches, everything, nullptr);
    for (const PxOverlapHit& hit : touches.all) {
      if (!hit.shape || !hit.actor) continue;
      ++stats.touched;
      const PxFilterData filter = hit.shape->getQueryFilterData();
      const PxGeometryType::Enum type = hit.shape->getGeometryType();
      const bool blocks = (filter.word1 >> options.channel) & 1, simple = filter.word3 & kSimpleCollision,
                 complex = filter.word3 & kComplexCollision;
      const uint32_t channel = ChannelOf(filter.word3);
      ++kinds[(static_cast<uint64_t>(type) << 16) | (channel << 8) | (simple ? 4 : 0) | (complex ? 2 : 0) | (blocks ? 1 : 0)];
      if (channel == kChannelPawn) {
        ++stats.characters;
        continue;
      }
      if (!blocks || !(options.complex ? complex : simple)) {
        ++stats.notSolid;
        continue;
      }
      const PxTransform pose = hit.actor->getGlobalPose() * hit.shape->getLocalPose();
      switch (type) {
        case PxGeometryType::eTRIANGLEMESH:
          ++stats.meshes;
          AddMesh(sink, *hit.shape, pose, scratch);
          break;
        case PxGeometryType::eCONVEXMESH:
          ++stats.convexes;
          AddConvex(sink, *hit.shape, pose, scratch);
          break;
        case PxGeometryType::eBOX:
          ++stats.boxes;
          AddBox(sink, *hit.shape, pose);
          break;
        case PxGeometryType::eSPHERE: {
          ++stats.spheres;
          PxSphereGeometry g;
          if (hit.shape->getSphereGeometry(g)) AddRound(sink, pose, g.radius, 0.0f);
          break;
        }
        case PxGeometryType::eCAPSULE: {
          ++stats.capsules;
          PxCapsuleGeometry g;
          if (hit.shape->getCapsuleGeometry(g)) AddRound(sink, pose, g.radius, g.halfHeight);
          break;
        }
        case PxGeometryType::eHEIGHTFIELD:
          ++stats.heightFields;  // not turned into triangles yet
          continue;
        default:
          ++stats.others;
          continue;
      }
      ++stats.used;
    }
    scene->unlockRead();
  }
  stats.triangles = static_cast<uint32_t>(out.size() / 9) - before;
  stats.full = sink.full;

  // The commonest kinds, for the log: "412 mesh ch0 simple+complex blocks".
  std::vector<std::pair<int, uint64_t>> order;
  for (const auto& kind : kinds) order.push_back({kind.second, kind.first});
  std::sort(order.rbegin(), order.rend());
  for (size_t i = 0; i < order.size() && i < 10; ++i) {
    const uint64_t key = order[i].second;
    char text[96];
    std::snprintf(text, sizeof(text), "%s%d %s ch%u%s%s%s", i ? "; " : "", order[i].first,
                  KindName(static_cast<PxGeometryType::Enum>(key >> 16)), static_cast<unsigned>((key >> 8) & 0xFF),
                  (key & 4) ? " simple" : "", (key & 2) ? " complex" : "", (key & 1) ? " blocks" : "");
    stats.kinds += text;
  }
  return true;
}

bool Below(const std::vector<float>& triangles, float x, float y, float z, float& height) {
  bool found = false;
  for (size_t at = 0; at + 9 <= triangles.size(); at += 9) {
    const float* t = &triangles[at];
    // Where (x, y) is in the triangle seen from above, as weights on its corners.
    const float ax = t[0], ay = t[1], bx = t[3], by = t[4], cx = t[6], cy = t[7];
    const float area = (bx - ax) * (cy - ay) - (cx - ax) * (by - ay);
    if (std::fabs(area) < 1e-6f) continue;
    const float u = ((bx - x) * (cy - y) - (cx - x) * (by - y)) / area;
    const float v = ((cx - x) * (ay - y) - (ax - x) * (cy - y)) / area;
    const float w = 1.0f - u - v;
    if (u < 0.0f || v < 0.0f || w < 0.0f) continue;
    const float h = u * t[2] + v * t[5] + w * t[8];
    if (h > z) continue;
    if (!found || h > height) height = h;
    found = true;
  }
  return found;
}

}  // namespace ground
