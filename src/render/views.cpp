#include "views.h"
#include "camera.h"
#include "ambientLight.h"
#include "directionalLight.h"
#include "fog.h"
#include "collisionNode.h"
#include "geomNode.h"
#include <cmath>

namespace render {

View::View(GraphicsOutput *win, NodePath root, bool o, int sort, const std::string &name) : ortho(o), win_(win) {
  PT(Camera) c = new Camera(name);
  if (o) { lens = new OrthographicLens(); lens->set_near_far(1, 20000); }
  else { auto *p = new PerspectiveLens(); p->set_fov(60); p->set_near_far(0.5, 20000); lens = p; }
  c->set_lens(lens);
  c->set_scene(root);
  cam = root.attach_new_node(c);
  dr_ = win->make_display_region(0, 0.01f, 0, 0.01f);
  dr_->set_sort(sort);
  dr_->set_camera(cam);
  dr_->set_clear_color_active(true); dr_->set_clear_depth_active(true);
  dr_->set_clear_color(LColor(0.35f, 0.45f, 0.6f, 1));
  if (o) { pitch = -90; yaw = 0; }
}
void View::setClear(const LColor &c) { dr_->set_clear_color(c); }
void View::setActive(bool a) { active_ = a; dr_->set_active(a); }
void View::setRect(const Rect &r, int W, int H) {
  rect_ = r; winW_ = std::max(W, 1); winH_ = std::max(H, 1);
  float l = r.x / winW_, rr = (r.x + r.w) / winW_, t = 1 - r.y / winH_, b = 1 - (r.y + r.h) / winH_;
  l = std::clamp(l, 0.f, 1.f); rr = std::clamp(rr, 0.f, 1.f); t = std::clamp(t, 0.f, 1.f); b = std::clamp(b, 0.f, 1.f);
  if (rr - l < 1e-3f || t - b < 1e-3f) { dr_->set_active(false); return; }
  dr_->set_dimensions(l, rr, b, t); dr_->set_active(active_);
  float aspect = std::max(r.w, 1.f) / std::max(r.h, 1.f);
  if (ortho) { float s = span; if (aspect >= 1) lens->set_film_size(s * aspect, s); else lens->set_film_size(s, s / aspect); }
  else lens->set_aspect_ratio(aspect);
}
void View::apply() {
  if (ortho) { cam.set_pos_hpr(target.get_x(), target.get_y(), 5000, 0, -90, 0); setRect(rect_, winW_, winH_); return; }
  float y = yaw * M_PI / 180, p = pitch * M_PI / 180;
  LVector3 fwd(sin(y) * cos(p), cos(y) * cos(p), sin(p));
  cam.set_pos(target - fwd * dist);
  // Past straight down/up, world +Z is behind the camera. Flip the up hint so the view can pass through the ground.
  cam.look_at(target, std::cos(p) >= 0 ? LVector3::up() : -LVector3::up());
  // Free-flight cameras can approach geometry at any map size.
  lens->set_near_far(0.05f, std::max(20000.f, dist * 50.f));
}
void View::frame(const LPoint3 &mn, const LPoint3 &mx) {
  target = (mn + mx) * 0.5f;
  if (ortho) { target.set_z(0); span = std::max(mx.get_x() - mn.get_x(), mx.get_y() - mn.get_y()) * 1.08f + 10; }
  else { target.set_z(mn.get_z()); dist = std::max(30.f, (mx - mn).length() * 0.75f); }
  apply();
}
bool View::ray(float px, float py, LPoint3 &from, LVector3 &dir) const {
  float nx = (px - rect_.x) / std::max(rect_.w, 1.f) * 2 - 1, ny = 1 - (py - rect_.y) / std::max(rect_.h, 1.f) * 2;
  LPoint3 nearP, farP;
  if (!lens->extrude(LPoint2(nx, ny), nearP, farP)) return false;
  NodePath top = cam.get_top();
  from = top.get_relative_point(cam, nearP);
  LPoint3 f = top.get_relative_point(cam, farP);
  dir = f - from; dir.normalize();
  return true;
}
bool View::project(const LPoint3 &w, float &px, float &py) const {
  LPoint3 c = cam.get_relative_point(cam.get_top(), w);
  LPoint2 s;
  if (!lens->project(c, s)) return false;
  px = rect_.x + (s.get_x() + 1) * 0.5f * rect_.w; py = rect_.y + (1 - s.get_y()) * 0.5f * rect_.h;
  return true;
}

void Lighting::attach(NodePath world) {
  world_ = world;
  PT(AmbientLight) a = new AmbientLight("amb"); amb_ = world.attach_new_node(a);
  PT(DirectionalLight) d = new DirectionalLight("sun"); sun_ = world.attach_new_node(d); sun_.set_hpr(35, -50, 0);
}
void Lighting::set(LightMode m, View *v3, View *v2) {
  mode = m;
  world_.clear_light(); world_.clear_fog(); world_.clear_color_scale();
  auto *a = DCAST(AmbientLight, amb_.node()); auto *d = DCAST(DirectionalLight, sun_.node());
  LColor bg(0.35f, 0.45f, 0.6f, 1);
  if (m == LightMode::Lit) {
    a->set_color(LColor(0.55f, 0.55f, 0.6f, 1)); d->set_color(LColor(0.75f, 0.72f, 0.66f, 1));
    world_.set_light(amb_); world_.set_light(sun_);
  } else if (m == LightMode::CogMood) {
    // Cog HQ style: smoggy purple-grey dusk, dim, fog.
    world_.set_color_scale(0.62f, 0.6f, 0.78f, 1);
    PT(Fog) f = new Fog("smog"); f->set_color(0.16f, 0.13f, 0.22f); f->set_exp_density(0.0022f);
    world_.set_fog(f);
    bg = LColor(0.13f, 0.11f, 0.19f, 1);
  }
  if (v3) v3->setClear(bg);
  if (v2) v2->setClear(m == LightMode::CogMood ? LColor(0.08f, 0.07f, 0.12f, 1) : LColor(0.07f, 0.08f, 0.1f, 1));
}

bool rayPlaneZ(const LPoint3 &o, const LVector3 &d, float z, LPoint3 &out) {
  if (fabs(d.get_z()) < 1e-6) return false;
  float t = (z - o.get_z()) / d.get_z(); if (t < 0) return false;
  out = o + d * t; return true;
}

PickHit pick(const View &v, float px, float py, NodePath root) {
  PickHit h; LPoint3 o; LVector3 d;
  if (!v.ray(px, py, o, d)) return h;
  static CollisionTraverser trav("pick"); static PT(CollisionHandlerQueue) q = new CollisionHandlerQueue();
  static PT(CollisionRay) ray = new CollisionRay(); static NodePath rnp;
  if (rnp.is_empty()) {
    PT(CollisionNode) cn = new CollisionNode("pickray"); cn->add_solid(ray);
    cn->set_from_collide_mask(GeomNode::get_default_collide_mask()); cn->set_into_collide_mask(0);
    rnp = NodePath(cn);
  }
  rnp.reparent_to(root.get_top());
  ray->set_origin(o); ray->set_direction(d);
  trav.clear_colliders(); q->clear_entries(); trav.add_collider(rnp, q);
  trav.traverse(root);
  rnp.detach_node();
  q->sort_entries();
  for (int i = 0; i < q->get_num_entries(); i++) {
    auto *e = q->get_entry(i);
    NodePath n = e->get_into_node_path();
    if (n.is_hidden()) continue;
    NodePath t = n.find_net_tag("eid");
    h.hit = true; h.point = e->get_surface_point(root.get_top()); h.t = (h.point - o).length();
    if (!t.is_empty()) { h.eid = atoi(t.get_net_tag("eid").c_str()); return h; }
    return h;
  }
  return h;
}

}  // namespace render
