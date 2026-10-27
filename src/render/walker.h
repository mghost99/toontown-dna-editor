#pragma once
#include "assets/animation.h"
#include "collisionTraverser.h"
#include "collisionHandlerQueue.h"
#include "collisionHandlerPusher.h"
#include "views.h"
#include <map>

namespace render {
class Walker {
 public:
  ~Walker() { stop(); }
  bool start(NodePath world, NodePath scene, const LPoint3 &position);
  void stop();
  bool active() const { return !root.is_empty(); }
  void step(NodePath scene, float dt, float forward, float turn, bool jump);
  void camera(View &view);
  NodePath root;
  bool grounded = false;
 private:
  NodePath legs_, torso_, head_, ray_;
  CollisionTraverser floorTest_, wallTest_;
  PT(CollisionHandlerQueue) floorHits_;
  PT(CollisionHandlerPusher) walls_;
  std::map<std::string, std::vector<PT(AnimControl)>> animations_;
  std::string playing_;
  float velocity_ = 0, lastFloor_ = 0;
  bool jumpHeld_ = false;
  void play(const std::string &name);
  bool floor(NodePath scene, float &height);
};
}
