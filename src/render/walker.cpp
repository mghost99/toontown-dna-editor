#include "walker.h"
#include "assets/storage.h"
#include "core/log.h"
#include "character.h"
#include "characterJoint.h"
#include "collisionRay.h"
#include "collisionSphere.h"
#include "collisionNode.h"
#include <algorithm>
#include <cmath>
#include <filesystem>

namespace render {
namespace {
std::string available(std::initializer_list<const char *> paths) {
  for (const char *path : paths) {
    std::error_code error;
    if (std::filesystem::is_regular_file(std::filesystem::path(assets::ModelCache::get().resRoot()) / path, error)) return path;
  }
  return "";
}
NodePath attachPoint(NodePath model, std::initializer_list<const char *> names) {
  for (const char *name : names) {
    NodePath point = model.find(std::string("**/") + name);
    if (!point.is_empty()) return point;
    NodePath character = model.find("**/+Character");
    if (!character.is_empty()) {
      auto actor = DCAST(Character, character.node());
      if (auto joint = actor->find_joint(name)) {
        point = character.attach_new_node(name); joint->add_net_transform(point.node()); return point;
      }
    }
  }
  return NodePath();
}
}
bool Walker::start(NodePath world, NodePath scene, const LPoint3 &position) {
  stop();
  std::string legs = available({"phase_3/models/char/toons/legs/tt_a_chr_dgs_shorts_legs_1000.bam", "phase_3/models/char/ttr_r_chr_dgs_legs.bam", "phase_3/models/char/tt_a_chr_dgs_shorts_legs_1000.bam"});
  std::string torso = available({"phase_3/models/char/toons/torso/tt_a_chr_dgs_shorts_torso_1000.bam", "phase_3/models/char/ttr_r_chr_dgs_torso.bam", "phase_3/models/char/tt_a_chr_dgs_shorts_torso_1000.bam"});
  std::string head = available({"phase_3/models/char/toons/head/tt_a_chr_dgs_shorts_head_1000.bam", "phase_3/models/char/ttr_r_chr_dgm_head.bam", "phase_3/models/char/tt_a_chr_dgs_shorts_head_1000.bam"});
  if (legs.empty() || torso.empty() || head.empty()) { core::warn("Walker needs the game's Toon legs, torso and head models in the resources folder."); return false; }
  NodePath legModel = assets::ModelCache::get().load(legs), torsoModel = assets::ModelCache::get().load(torso), headModel = assets::ModelCache::get().load(head);
  if (legModel.is_empty() || torsoModel.is_empty() || headModel.is_empty()) return false;
  root = world.attach_new_node("editor-walker");
  legs_ = legModel.copy_to(root); torso_ = torsoModel.copy_to(root); head_ = headModel.copy_to(root);
  auto pelvis = attachPoint(legs_, {"joint_hips", "jnt_M_attachPelvis_01"});
  auto neck = attachPoint(torso_, {"def_head", "def_M_head_01"});
  if (pelvis.is_empty() || neck.is_empty()) { core::warn("Walker could not find the Toon body attachment joints."); stop(); return false; }
  torso_.reparent_to(pelvis); head_.reparent_to(neck);
  for (const char *pattern : {"**/shoes", "**/boots*", "**/*naked*", "**/*skirt*"}) root.find_all_matches(pattern).hide();
  for (auto part : {legs_, torso_, head_}) {
    part.find_all_matches("**/+CollisionNode").stash();
    for (const char *pattern : {"**/legs", "**/feet", "**/arms", "**/neck", "**/head", "**/head-front", "**/ears"}) {
      auto nodes = part.find_all_matches(pattern);
      for (int i = 0; i < nodes.get_num_paths(); ++i) nodes[i].set_color(0.3f, 0.65f, 0.95f, 1);
    }
  }
  // Use the same separate body-part binding as the C++ client concept. Clash
  // stores its rig and animation files together; classic resources use phases.
  for (const auto &entry : {std::make_pair(legs_, legs), std::make_pair(torso_, torso), std::make_pair(head_, head)}) {
    for (const char *state : {"neutral", "run"}) {
      std::string base = std::filesystem::path(entry.second).stem().string();
      if (base.size() > 5 && base.substr(base.size() - 5) == "_1000") base.resize(base.size() - 5);
      if (auto index = base.find("_r_"); index != std::string::npos) base.replace(index, 3, "_a_");
      auto controls = assets::loopModel(entry.first, entry.second, base + "_" + state);
      for (auto &control : controls) control->stop();
      auto &target = animations_[state]; target.insert(target.end(), controls.begin(), controls.end());
    }
  }
  PT(CollisionNode) feet = new CollisionNode("walker-floor");
  feet->set_from_collide_mask(BitMask32::all_on()); feet->set_into_collide_mask(BitMask32::all_off());
  feet->add_solid(new CollisionRay(0, 0, 2.5f, 0, 0, -1)); ray_ = root.attach_new_node(feet); ray_.hide();
  floorHits_ = new CollisionHandlerQueue(); floorTest_.add_collider(ray_, floorHits_);
  PT(CollisionNode) body = new CollisionNode("walker-body");
  body->set_from_collide_mask(BitMask32(1)); body->set_into_collide_mask(BitMask32::all_off());
  body->add_solid(new CollisionSphere(0, 0, 1.4f, 0.7f)); NodePath bodyPath = root.attach_new_node(body); bodyPath.hide();
  walls_ = new CollisionHandlerPusher(); walls_->set_horizontal(true); walls_->add_collider(bodyPath, root); wallTest_.add_collider(bodyPath, walls_);
  root.set_pos(position); lastFloor_ = position[2]; velocity_ = 0;
  float height; if (floor(scene, height)) { root.set_z(height); lastFloor_ = height; grounded = true; }
  play("neutral"); return true;
}
void Walker::stop() {
  for (auto &[name, controls] : animations_) assets::stopAnimations(controls);
  animations_.clear(); playing_.clear();
  floorTest_.clear_colliders(); wallTest_.clear_colliders();
  if (!root.is_empty()) root.remove_node();
  root = NodePath(); grounded = false; jumpHeld_ = false; velocity_ = 0;
}
void Walker::play(const std::string &name) {
  if (playing_ == name) return;
  for (auto &[key, controls] : animations_) for (auto &control : controls) control->stop();
  for (auto &control : animations_[name]) control->loop(true);
  playing_ = name;
}
bool Walker::floor(NodePath scene, float &height) {
  floorHits_->clear_entries(); floorTest_.traverse(scene); floorHits_->sort_entries();
  for (int i = 0; i < floorHits_->get_num_entries(); ++i) {
    auto hit = floorHits_->get_entry(i);
    if (!hit->has_surface_point() || !hit->has_surface_normal()) continue;
    if (hit->get_surface_normal(root.get_parent())[2] < 0.5f) continue;
    height = hit->get_surface_point(root.get_parent())[2]; return true;
  }
  return false;
}
void Walker::step(NodePath scene, float dt, float forward, float turn, bool jump) {
  if (!active()) return;
  dt = std::clamp(dt, 0.f, 0.1f);
  if (jump && !jumpHeld_ && grounded) { velocity_ = 14.f; grounded = false; }
  jumpHeld_ = jump;
  int steps = std::max(1, (int)std::ceil(dt * 120)); float delta = dt / steps;
  for (int i = 0; i < steps; ++i) {
    root.set_h(root.get_h() + turn * 100.f * delta);
    // Panda Toons face local +Y; positive heading turns left.
    root.set_y(root, forward * 12.f * delta);
    wallTest_.traverse(scene);
    velocity_ -= 32.f * delta; root.set_z(root.get_z() + velocity_ * delta);
    float height;
    if (floor(scene, height) && velocity_ <= 0 && root.get_z() <= height + 0.15f) {
      root.set_z(height); velocity_ = 0; grounded = true; lastFloor_ = height;
    } else grounded = false;
    if (root.get_z() < lastFloor_ - 100) { root.set_z(lastFloor_ + 5); velocity_ = 0; }
  }
  play(grounded && std::fabs(forward) > 0.01f ? "run" : "neutral");
}
void Walker::camera(View &view) {
  if (!active()) return;
  float heading = root.get_h() * float(M_PI) / 180;
  LVector3 forward(-std::sin(heading), std::cos(heading), 0);
  view.target = root.get_pos() + LVector3(0, 0, 2.5f);
  view.cam.set_pos(view.target - forward * 16.f + LVector3(0, 0, 6));
  view.cam.look_at(view.target); view.lens->set_near_far(0.05f, 20000);
}
}
