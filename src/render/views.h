// Cameras + display regions for the 3D view, the top-down 2D map, and the prop preview; lighting modes; picking.
#pragma once
#include "graphicsOutput.h"
#include "displayRegion.h"
#include "nodePath.h"
#include "perspectiveLens.h"
#include "orthographicLens.h"
#include "collisionTraverser.h"
#include "collisionHandlerQueue.h"
#include "collisionRay.h"

namespace render {

struct Rect { float x = 0, y = 0, w = 0, h = 0; };   // in window pixels, y down (ImGui coordinates)

class View {
 public:
  View(GraphicsOutput *win, NodePath sceneRoot, bool ortho, int sort, const std::string &name);
  void setRect(const Rect &r, int winW, int winH);   // places the display region under an ImGui window
  void setActive(bool a);
  bool active() const { return active_; }
  Rect rect() const { return rect_; }
  NodePath cam;
  PT(Lens) lens;
  bool ortho;
  // orbit/2D parameters
  LPoint3 target{0, 0, 0}; float dist = 200, yaw = 30, pitch = -35;   // 3D orbit (pitch negative = looking down)
  float span = 300;                                                     // 2D: world units across the shorter side
  void apply();                                                          // push params to camera
  void frame(const LPoint3 &mn, const LPoint3 &mx);                     // fit bounds
  // mouse helpers (pixel coords inside the window)
  bool ray(float px, float py, LPoint3 &from, LVector3 &dir) const;     // world-space ray
  bool project(const LPoint3 &world, float &px, float &py) const;       // world -> window pixels
  void setClear(const LColor &c);
 private:
  PT(DisplayRegion) dr_;
  GraphicsOutput *win_;
  Rect rect_; bool active_ = true;
  int winW_ = 1, winH_ = 1;
};

enum class LightMode { Game = 0, Lit = 1, CogMood = 2 };
class Lighting {
 public:
  void attach(NodePath world);
  void set(LightMode m, View *v3d, View *v2d);
  LightMode mode = LightMode::Game;
 private:
  NodePath world_, amb_, sun_; NodePath fogNp_;
};

struct PickHit { NodePath node; int eid = -1; LPoint3 point; LVector3 normal{0, 0, 1}; bool hit = false; float t = 1e30f; };
// World-space ray cast against root (first visible hit, with surface normal).
PickHit castRay(NodePath root, const LPoint3 &o, const LVector3 &d, float maxDist = 1e9f);
PickHit pick(const View &v, float px, float py, NodePath root, NodePath skip = NodePath());
bool rayPlaneZ(const LPoint3 &o, const LVector3 &d, float z, LPoint3 &out);

}  // namespace render
