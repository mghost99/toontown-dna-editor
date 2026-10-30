#include "editor.h"
#include "core/log.h"
#include "core/resconfig.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "ImGuizmo.h"
#include "ambientLight.h"
#include "directionalLight.h"
#include "geomNode.h"
#include "geom.h"
#include "geomPrimitive.h"
#include "config_putil.h"
#include "textNode.h"
#include "billboardEffect.h"
#include "clockObject.h"
#include "texture.h"
#include "cardMaker.h"
#include "transparencyAttrib.h"
#include "nodePathCollection.h"
#include "depthOffsetAttrib.h"
#include "colorBlendAttrib.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <vector>
#include <regex>
#include <set>

namespace fs = std::filesystem;
using core::ScopedTimer;

static std::string fmt(const char *f, ...) {
  char b[1024]; va_list a; va_start(a, f); vsnprintf(b, sizeof b, f, a); va_end(a); return b;
}

Editor::Editor(platform::App &app, std::string res) : resRoot(std::move(res)), app_(app) {
  std::string speed = core::readConfig("camera_wheel_speed");
  if (!speed.empty()) { float value = strtof(speed.c_str(), nullptr); if (std::isfinite(value)) wheelSpeed = std::clamp(value, 0.1f, 50.f); }
  { std::string ic = core::readConfig("invert_camera"); if (ic == "0") invertCamera_ = false; else if (ic == "1") invertCamera_ = true; }
  { std::string iy = core::readConfig("invert_camera_y"); if (iy == "0") invertCameraY_ = false; else if (iy == "1") invertCameraY_ = true; }
  assets::ModelCache::get().setResRoot(resRoot);
  get_model_path().prepend_directory(Filename::from_os_specific(resRoot));
  GraphicsOutput *w = app.win();
  v3 = std::make_unique<render::View>(w, scene.world, false, 10, "cam3d");
  v2 = std::make_unique<render::View>(w, scene.world, true, 20, "cam2d");
  previewRoot_ = NodePath("preview");
  pv = std::make_unique<render::View>(w, previewRoot_, false, 30, "campv");
  pv->setClear(LColor(0.13f, 0.14f, 0.17f, 1)); pv->setActive(false);
  PT(AmbientLight) a = new AmbientLight("pa"); a->set_color(LColor(0.6f, 0.6f, 0.65f, 1));
  PT(DirectionalLight) d = new DirectionalLight("pd"); d->set_color(LColor(0.7f, 0.68f, 0.62f, 1));
  NodePath an = previewRoot_.attach_new_node(a), dn = previewRoot_.attach_new_node(d); dn.set_hpr(30, -45, 0);
  previewRoot_.set_light(an); previewRoot_.set_light(dn);
  light_.attach(scene.world);
  light_.set(render::LightMode::Game, v3.get(), v2.get());
  history.onChanged = [this](const std::vector<int> &ids) { if (ids.empty()) rebuildScene(); else syncItems(ids); };
  { ScopedTimer t("catalog scan"); catalog = dna::loadCatalog(resRoot); }
  core::info(fmt("palette: %d props from all storage files", (int)catalog.size()));
  files_ = dna::sceneFiles(resRoot);
}

bool Editor::open(const std::string &f) {
  ScopedTimer total("open DNA (total)");
  std::string abs = fs::path(f).is_absolute() ? f : (fs::exists(f) ? fs::absolute(f).string() : resRoot + "/" + f);
  std::string err;
  dna::Document nd;
  { ScopedTimer t("parse DNA");
    if (!nd.load(abs, resRoot, &err)) {
      if (nd.loadError == dna::LoadError::NeedPassword || nd.loadError == dna::LoadError::WrongPassword) {
        pwFile_ = abs; pwMsg_ = nd.loadError == dna::LoadError::WrongPassword ? "Wrong password, try again." : ""; showPw_ = true;
        core::warn(fs::path(abs).filename().string() + " is encrypted: enter its password");
      } else core::error(err);
      return false;
    } }
  if (walker.active()) toggleWalker();
  doc = std::move(nd);
  std::vector<std::string> undef;
  auto st = dna::storagesForFile(resRoot, doc.rel, doc.text, &undef);
  storage_.clear(); storage_.addFiles(st);
  std::string sl; for (auto &s : st) sl += (sl.empty() ? "" : ", ") + fs::path(s).filename().string();
  core::info("storage for " + doc.rel + ": " + sl);
  for (auto &u : undef) {
    // Appearance Override ADDs that also carry model= load their BAM directly (game + editor);
    // a missing storage code there is fine, so only warn for code-only uses.
    bool hasModel = false;
    dna::walk(doc.root.get(), [&](const dna::XmlNode *n) {
      if (!hasModel && n->attr("code") == u && !n->attr("model").empty()) hasModel = true; });
    if (!hasModel) core::warn("code used by this file but defined in no storage file: " + u);
  }
  history.clear(); selection.clear(); boxes_.clear(); editing_ = false; dragging_ = dragArmed_ = false; overrideDirty_ = false; ovFxLoaded_ = false;
  detachContext(); ctxRel_.clear(); render::setHostDnaOverride("");
  { ScopedTimer t("generate scene (models+textures)"); scene.build(doc, storage_); }
  scanContextAreas(); rebuildContext();
  vgVisible_.clear(); for (auto &v : doc.visgroups) vgVisible_[v] = true;
  loadedCodes_ = storage_.codes();
  int nested = 0; for (auto &it : doc.items) nested += it.nested;
  core::info(fmt("opened %s: %d editable objects (%d nested), %d visgroups, %d scene nodes, %d models loaded",
                 doc.rel.c_str(), (int)doc.items.size(), nested, (int)doc.visgroups.size(), scene.stats_nodes,
                 (int)assets::ModelCache::get().loadedCount()));
  frameAll();
  core::writeConfig("last_file", fs::absolute(abs).lexically_normal().string());
  app_.fw.set_window_title("DNA Editor - " + doc.rel + " [" + dna::formatName(doc.fmt) + (doc.encrypted ? ", encrypted" : "") + "]");
  return true;
}

bool Editor::save(const std::string &outDir) {
  if (doc.path.empty()) return false;
  ScopedTimer t("save");
  std::string out = outDir.empty() ? "" : (fs::path(outDir) / fs::path(doc.path).filename()).string();
  if (!outDir.empty()) fs::create_directories(outDir);
  std::string err; dna::SaveStats s;
  if (!doc.save(&err, &s, out)) { core::error("save failed: " + err); return false; }
  overrideDirty_ = false;
  core::info(fmt("saved %s (%d moved, %d deleted, %d added; backup %s.bak)", (out.empty() ? doc.path : out).c_str(),
                 s.moved, s.deleted, s.added, fs::path(out.empty() ? doc.path : out).filename().string().c_str()));
  if (out.empty()) {   // re-read so ids/spans match the new text; models are cached so this is fast
    auto cam3 = std::make_tuple(v3->target, v3->dist, v3->yaw, v3->pitch); auto cam2 = std::make_tuple(v2->target, v2->span);
    open(doc.path);
    std::tie(v3->target, v3->dist, v3->yaw, v3->pitch) = cam3; std::tie(v2->target, v2->span) = cam2; v3->apply(); v2->apply();
  }
  return true;
}

bool Editor::saveAs(const std::string &p) {
  if (doc.path.empty() || p.empty()) return false;
  std::string err; dna::SaveStats s;
  if (!doc.save(&err, &s, p)) { core::error("save as failed: " + err); return false; }
  std::string ex; if (doc.lastCompile.skipped && fs::path(p).extension() == ".pdna")
    ex = fmt(" (%d elements not representable in .pdna were left out: %s)", doc.lastCompile.skipped, doc.lastCompile.skippedTags.c_str());
  core::info("saved as " + p + ex);
  if (ex.empty() || fs::path(p).extension() != ".pdna") open(p);   // continue editing the new file
  return true;
}

void Editor::passwordDialog() {
  if (showPw_) { ImGui::OpenPopup("Encrypted DNA file"); showPw_ = false; }
  if (ImGui::BeginPopupModal("Encrypted DNA file", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::Text("%s is encrypted.", fs::path(pwFile_).filename().string().c_str());
    ImGui::TextDisabled("The password is kept in memory for this session only.");
    if (!pwMsg_.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.35f, 1), "%s", pwMsg_.c_str());
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(320);
    bool enter = ImGui::InputText("Password", pwBuf_, sizeof pwBuf_, ImGuiInputTextFlags_Password | ImGuiInputTextFlags_EnterReturnsTrue);
    auto wipe = [&]() { volatile char *p = pwBuf_; for (size_t i = 0; i < sizeof pwBuf_; i++) p[i] = 0; };
    if (ImGui::Button("OK", ImVec2(100, 0)) || enter) {
      dna::setSessionPassword(pwBuf_); wipe(); ImGui::CloseCurrentPopup();
      std::string f = pwFile_; open(f);   // reopens the prompt with an error if the password is wrong
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(100, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) { wipe(); pwMsg_.clear(); ImGui::CloseCurrentPopup(); }
    ImGui::EndPopup();
  }
}

void Editor::rebuildScene() {
  detachContext();
  scene.build(doc, storage_);
  attachContext();
  for (int i = 0; i < (int)doc.items.size(); ++i) {
    if (doc.items[i].origId < 0) scene.buildAdded(doc, i, storage_);
    scene.syncItem(doc, i);
  }
  for (auto &[zone, visible] : vgVisible_) scene.setVisgroupVisible(zone, visible);
  boxes_.clear();
}

void Editor::syncItems(const std::vector<int> &ids) {
  for (int i : ids) scene.syncItem(doc, i);
  for (auto it = selection.begin(); it != selection.end();)
    if (doc.items[*it].deleted) it = selection.erase(it); else ++it;
}

void Editor::select(const std::vector<int> &ids) { selection = ids; }

void Editor::pushXformFromNode(int id, bool fromGizmo) {
  NodePath n = scene.itemNodes[id]; auto &x = doc.items[id].xf;
  LPoint3 p = n.get_pos(); LVecBase3 h = n.get_hpr(), s = n.get_scale();
  x.x = p[0]; x.y = p[1]; x.z = p[2]; x.h = h[0]; x.p = h[1]; x.r = h[2]; x.sx = s[0]; x.sy = s[1]; x.sz = s[2];
  if (fromGizmo && snapActive()) {   // float noise from matrix decomposition: land exactly on the snap steps
    auto q = [](double v, double st) { double r = std::round(v / st) * st; return std::fabs(r - v) < st * 0.05 ? r : v; };
    if (gizmoOp == render::GizmoOp::Rotate) { x.h = q(x.h, snap.angle); x.p = q(x.p, snap.angle); x.r = q(x.r, snap.angle); }
    if (gizmoOp == render::GizmoOp::Translate) { x.x = q(x.x, snap.move); x.y = q(x.y, snap.move); x.z = q(x.z, snap.move); }
    if (gizmoOp == render::GizmoOp::Scale) { x.sx = q(x.sx, snap.scale); x.sy = q(x.sy, snap.scale); x.sz = q(x.sz, snap.scale); }
    scene.syncItem(doc, id);
  }
}

void Editor::beginEdit() {
  editIds_ = selection; editBefore_.clear();
  for (int i : editIds_) editBefore_.push_back(doc.items[i].xf);
  editing_ = true;
}
void Editor::endEdit(const char *label) {
  if (!editing_) return;
  editing_ = false;
  auto c = std::make_unique<commands::TransformCmd>(); c->what = label;
  bool any = false;
  for (size_t k = 0; k < editIds_.size(); k++) {
    c->ids.push_back(editIds_[k]); c->before.push_back(editBefore_[k]); c->after.push_back(doc.items[editIds_[k]].xf);
    if (editBefore_[k] != doc.items[editIds_[k]].xf) any = true;
  }
  if (any) history.pushApplied(std::move(c));
}

void Editor::yawSelectedBy(float deg, bool commit) {
  if (selection.empty()) return;
  if (commit) beginEdit();
  for (int i : selection) {
    auto &x = doc.items[i].xf; double h = x.h + deg;
    if (snap.on && snap.angle > 0) h = std::round(h / snap.angle) * snap.angle;
    h = std::fmod(h, 360.0); if (h > 180) h -= 360; if (h <= -180) h += 360;
    x.h = h; scene.syncItem(doc, i);
  }
  if (commit) endEdit("Rotate");
}

void Editor::pitchSelectedBy(float deg, bool commit) {
  if (selection.empty()) return;
  if (commit) beginEdit();
  for (int i : selection) {
    auto &x = doc.items[i].xf; double p = x.p + deg;
    if (snap.on && snap.angle > 0) p = std::round(p / snap.angle) * snap.angle;
    x.p = p; scene.syncItem(doc, i);
  }
  if (commit) endEdit("Rotate");
}

void Editor::scaleSelectedUniform(float factor, bool commit) {
  if (selection.empty() || factor <= 0) return;
  if (commit) beginEdit();
  for (int i : selection) {
    auto &x = doc.items[i].xf;
    x.sx = std::max(0.01, x.sx * factor);
    x.sy = std::max(0.01, x.sy * factor);
    x.sz = std::max(0.01, x.sz * factor);
    scene.syncItem(doc, i);
  }
  if (commit) endEdit("Scale");
}

bool Editor::consumeSelectionWheel(const ImGuiIO &io) {
  if (selection.empty() || io.MouseWheel == 0.f || wheelUsed_) return false;
  const bool shift = io.KeyShift, ctrl = io.KeyCtrl, alt = io.KeyAlt;
  if (!shift && !ctrl) return false;
  const float w = io.MouseWheel;
  if (ctrl && alt) {
    pitchSelectedBy((w > 0 ? 1.f : -1.f) * wheelAngleStep() * std::max(1.f, std::fabs(w)), true);
  } else if (shift) {
    // ~5% per notch, multiplicative (fractional wheels scale proportionally)
    scaleSelectedUniform(std::pow(1.05f, w), true);
  } else if (ctrl) {
    yawSelectedBy((w > 0 ? 1.f : -1.f) * wheelAngleStep() * std::max(1.f, std::fabs(w)), true);
  } else {
    return false;
  }
  wheelUsed_ = true;
  return true;
}

void Editor::moveSelectedBy(const LVector3 &d, bool commit) {
  if (selection.empty()) return;
  if (commit) beginEdit();
  NodePath top = scene.world;
  for (int i : selection) { NodePath n = scene.itemNodes[i]; n.set_pos(top, n.get_pos(top) + d); pushXformFromNode(i); }
  if (commit) endEdit("Move");
}

void Editor::duplicateSelected() {
  std::vector<int> added;
  for (int i : selection) {
    const auto &src = doc.items[i];
    if (src.nested) { core::warn("duplicate: '" + src.name + "' is nested inside another object; duplicate its parent instead"); continue; }
    dna::Item it = src; it.origId = -1; it.node = nullptr; it.deleted = false;
    it.templateXml = doc.itemXml(src);   // always XML (the document itself may be .dna/.pdna)
    it.origXf = src.origId >= 0 ? src.origXf : src.origXf;
    it.xf.x += snap.on ? std::max(1.0f, snap.move) * 2 : 2; it.xf.y += 2;
    doc.items.push_back(it); int id = (int)doc.items.size() - 1;
    scene.buildAdded(doc, id, storage_); added.push_back(id);
  }
  if (added.empty()) return;
  auto c = std::make_unique<commands::DeleteCmd>(); c->ids = added; c->add = true;
  history.pushApplied(std::move(c));
  selection = added;
  core::info(fmt("duplicated %d object(s)", (int)added.size()));
}

void Editor::deleteSelected() {
  if (selection.empty()) return;
  auto c = std::make_unique<commands::DeleteCmd>(); c->ids = selection;
  history.execute(doc, std::move(c));
  selection.clear();
}

LPoint3 Editor::placementPoint() {
  render::Rect r = v3->rect();
  render::PickHit h = render::pick(*v3, r.x + r.w / 2, r.y + r.h / 2, scene.geom);
  if (h.hit) return h.point;
  LPoint3 o, p; LVector3 d;
  if (v3->ray(r.x + r.w / 2, r.y + r.h / 2, o, d) && render::rayPlaneZ(o, d, 0, p)) return p;
  return v3->target;
}

bool Editor::openPreview(int ci) {
  if (ci < 0 || ci >= (int)catalog.size()) return false;
  ScopedTimer t("open prop preview");
  auto &e = catalog[ci];
  assets::stopAnimations(previewAnimations_); previewAnimation_.clear();
  if (!previewModel_.is_empty()) previewModel_.remove_node();
  NodePath src = assets::loadStoreNode(e.model, e.node);
  previewIdx_ = ci; pvYaw_ = 35; pvPitch_ = 20; pvZoom_ = 1;
  if (src.is_empty()) { core::warn("preview: cannot load " + e.code + " (" + e.model + ")"); previewModel_ = previewRoot_.attach_new_node("empty"); previewSize_ = LVecBase3(0); previewTris_ = 0; return false; }
  previewModel_ = src.copy_to(previewRoot_);
  std::string animation;
  if (doc.root) dna::walk(doc.root.get(), [&](const dna::XmlNode *node) {
    if (node->attr("code") == e.code && !node->attr("anim").empty()) animation = node->attr("anim");
  });
  previewAnimations_ = assets::loopModel(previewModel_, e.model, animation, &previewAnimation_);
  previewModel_.find_all_matches("**/+CollisionNode").hide();
  previewModel_.set_pos_hpr_scale(0, 0, 0, 0, 0, 0, 1, 1, 1);
  LPoint3 mn, mx;
  if (!previewModel_.calc_tight_bounds(mn, mx)) mn = mx = LPoint3(0);
  previewSize_ = mx - mn;
  previewTris_ = 0;
  NodePathCollection gs = previewModel_.find_all_matches("**/+GeomNode");
  for (int gi = 0; gi < gs.get_num_paths(); gi++) { NodePath g = gs.get_path(gi);
    GeomNode *gn = DCAST(GeomNode, g.node());
    for (int i = 0; i < gn->get_num_geoms(); i++) {
      CPT(Geom) geo = gn->get_geom(i);
      for (size_t k = 0; k < geo->get_num_primitives(); k++) previewTris_ += geo->get_primitive(k)->get_num_faces();
    }
  }
  pv->target = (mn + mx) * 0.5f;
  return true;
}

int Editor::placePreviewed() {
  if (previewIdx_ < 0 || doc.path.empty()) return -1;
  auto &e = catalog[previewIdx_];
  // Appearance Override ADDs carry model= so the game can load the BAM even when the
  // hood's storage files don't define the code (e.g. new TTEX Halloween props).
  const bool ovModel = isAppearanceOverride() && !e.model.empty();
  if (!loadedCodes_.count(e.code)) {
    storage_.addCode(e.code, e.model, e.node, e.storageRel); loadedCodes_.insert(e.code);
    if (!ovModel)
      core::warn("placed " + e.code + " from " + e.storageRel + ", which this DNA's storage files do not load: the game will not show it unless that storage is added");
  }
  if (previewAnimation_.empty() && !previewAnimations_.empty())
    core::warn("placed " + e.code + " as a plain prop: its animations are embedded in the model, so no anim name is written");
  dna::Item it; it.kind = previewAnimation_.empty() ? "prop" : (e.root == "interactive_prop" ? "interactive_prop" : "anim_prop");
  it.code = e.code; it.name = e.code;
  it.visgroup = !selection.empty() ? doc.items[selection[0]].visgroup : (doc.visgroups.empty() ? "" : doc.visgroups[0]);
  it.templateXml = "<" + it.kind + " name=\"" + e.code + "\" code=\"" + e.code + "\"";
  if (ovModel) it.templateXml += " model=\"" + e.model + "\"";
  if (!previewAnimation_.empty()) it.templateXml += " anim=\"" + previewAnimation_ + "\"";
  if (it.kind == "interactive_prop") it.templateXml += " cell_id=\"0\"";
  // ATTACH-TO-MODEL: when editing an Appearance Override, stamp parent_code so the
  // game parents this ADD to the named DNA prop (pos/hpr/scale stay local).
  if (isAppearanceOverride() && ovParentCode_[0]) {
    it.templateXml += " parent_code=\"" + std::string(ovParentCode_) + "\"";
    if (ovParentNode_[0])
      it.templateXml += " parent_node=\"" + std::string(ovParentNode_) + "\"";
  }
  it.templateXml += ">\n\t<pos x=\"0.00\" y=\"0.00\" z=\"0.00\" />\n\t<nhpr h=\"0.00\" p=\"0.00\" r=\"0.00\" />\n</" + it.kind + ">";
  LPoint3 p = placementPoint();
  it.xf.x = snapv(p[0]); it.xf.y = snapv(p[1]); it.xf.z = p[2];
  doc.items.push_back(it); int id = (int)doc.items.size() - 1;
  scene.buildAdded(doc, id, storage_);
  auto c = std::make_unique<commands::DeleteCmd>(); c->ids = {id}; c->add = true; history.pushApplied(std::move(c));
  selection = {id};
  core::info(fmt("placed %s at (%.1f, %.1f, %.1f) in visgroup %s", e.code.c_str(), it.xf.x, it.xf.y, it.xf.z, it.visgroup.c_str()));
  return id;
}

void Editor::setLight(render::LightMode m) { light_.set(m, v3.get(), v2.get()); }
void Editor::frameAll() { scene.computeBounds(); v3->frame(scene.boundsMin, scene.boundsMax); v2->frame(scene.boundsMin, scene.boundsMax); }
void Editor::frameSelection() {
  if (selection.empty()) { frameAll(); return; }
  LPoint3 mn(1e9), mx(-1e9); NodePath top = scene.world;
  for (int i : selection) { LPoint3 a, b; if (scene.itemNodes[i].calc_tight_bounds(a, b, top)) { mn = mn.fmin(a); mx = mx.fmax(b); } else { LPoint3 p = scene.itemNodes[i].get_pos(top); mn = mn.fmin(p); mx = mx.fmax(p); } }
  v3->target = (mn + mx) * 0.5f; v3->dist = std::max(15.f, (mx - mn).length() * 1.6f); v3->apply();
  v2->target = v3->target; v2->span = std::max(40.f, std::max(mx[0] - mn[0], mx[1] - mn[1]) * 3); v2->apply();
}
float Editor::snapv(float v) const {
  // Hold Alt while dragging to temporarily bypass snap (cobwebs overlapping the host, etc.).
  if (ImGui::GetIO().KeyAlt) return v;
  return snap.on && snap.move > 0 ? std::round(v / snap.move) * snap.move : v;
}
bool Editor::snapActive() const { return snap.on && !ImGui::GetIO().KeyAlt; }

const Editor::Box &Editor::boxOf(int i) {
  auto &b = boxes_[i];
  if (!b.ok) { NodePath n = scene.itemNodes[i]; if (n.is_empty() || !n.calc_tight_bounds(b.mn, b.mx, n)) { b.mn = LPoint3(-1); b.mx = LPoint3(1); }
    for (int a = 0; a < 3; a++) if (b.mx[a] - b.mn[a] < 1.0f) { float c = (b.mx[a] + b.mn[a]) * 0.5f; b.mn[a] = c - 0.5f; b.mx[a] = c + 0.5f; }   // tiny/flat: min 1 ft grab box
    b.ok = true; }
  return b;
}
// ray vs the item's oriented bounding box (slab test in the item's local frame; t is world distance)
bool Editor::rayBox(int i, const LPoint3 &o, const LVector3 &d, float &tEnter) {
  NodePath n = scene.itemNodes[i]; if (n.is_empty() || n.is_hidden() || doc.items[i].deleted) return false;
  const Box &b = boxOf(i);
  LMatrix4 inv; inv.invert_from(n.get_mat(scene.world));
  LPoint3 lo = inv.xform_point(o); LVector3 ld = inv.xform_vec(d);
  float t0 = -1e30f, t1 = 1e30f;
  for (int a = 0; a < 3; a++) {
    if (std::fabs(ld[a]) < 1e-9f) { if (lo[a] < b.mn[a] || lo[a] > b.mx[a]) return false; continue; }
    float ta = (b.mn[a] - lo[a]) / ld[a], tb = (b.mx[a] - lo[a]) / ld[a]; if (ta > tb) std::swap(ta, tb);
    t0 = std::max(t0, ta); t1 = std::min(t1, tb); if (t0 > t1) return false;
  }
  if (t1 < 0) return false;
  tEnter = std::max(t0, 0.f);   // affine map keeps the ray parameter; d is unit length in world space
  return true;
}
int Editor::pickItem(render::View &v, float px, float py, LPoint3 &hitPt) {
  LPoint3 o; LVector3 d; if (!v.ray(px, py, o, d)) return -1;
  // 1) geometry under the cursor
  render::PickHit h = render::pick(v, px, py, scene.geom);
  // 2) prefer a smaller object whose box the ray enters before reaching that surface (gaps, thin props,
  //    things standing on a big ground/building piece), so the object you see under the cursor is chosen
  int best = h.eid; float bestVol = 1e30f, tg = h.hit ? h.t : 1e30f;
  auto vol = [&](int i) { const Box &b = boxOf(i); LVecBase3 s = scene.itemNodes[i].get_scale(scene.world);
    return std::fabs((b.mx[0] - b.mn[0]) * s[0] * (b.mx[1] - b.mn[1]) * s[1] * (b.mx[2] - b.mn[2]) * s[2]); };
  if (best >= 0) bestVol = vol(best);
  for (int i = 0; i < (int)doc.items.size(); i++) {
    if (i == h.eid) continue; float t;
    if (!rayBox(i, o, d, t) || t > tg + 0.01f) continue;
    float vv = vol(i); if (vv < bestVol) { bestVol = vv; best = i; }
  }
  // 3) a selected object keeps the click inside its outline box only if it is no bigger than what would be picked
  //    otherwise (so a selected playground/street model never swallows clicks on the props standing on it)
  for (int i : selection) {
    float t; if (!rayBox(i, o, d, t)) continue;
    if (best < 0 || i == best || vol(i) <= bestVol) { hitPt = o + d * t; if (i == best && h.hit && best == h.eid) hitPt = h.point; return i; }
  }
  if (best >= 0) { float t; hitPt = (best == h.eid && h.hit) ? h.point : (rayBox(best, o, d, t) ? o + d * t : h.point); }
  return best;
}

int Editor::pickAt3D(float px, float py) {
  ScopedTimer t("pick (select)", false);
  return render::pick(*v3, px, py, scene.geom).eid;
}

// ----------------------------------------------------------------------------------------------------------------
void Editor::drawSelection(render::View &v, bool topDown) {
  ImDrawList *dl = ImGui::GetWindowDrawList();
  NodePath top = scene.world;
  for (size_t k = 0; k < selection.size(); k++) {
    int i = selection[k]; NodePath n = scene.itemNodes[i]; if (n.is_empty()) continue;
    const Box &b = boxOf(i);
    LMatrix4 m = n.get_mat(top);
    ImVec2 pts[8]; bool ok = true;
    for (int c = 0; c < 8; c++) {
      LPoint3 p((c & 1) ? b.mx[0] : b.mn[0], (c & 2) ? b.mx[1] : b.mn[1], (c & 4) ? b.mx[2] : b.mn[2]);
      p = m.xform_point(p); float x, y;
      if (!v.project(p, x, y)) { ok = false; break; } pts[c] = ImVec2(x, y);
    }
    if (!ok) continue;
    ImU32 col = k == 0 ? IM_COL32(255, 200, 60, 255) : IM_COL32(120, 200, 255, 230);
    int e[12][2] = {{0, 1}, {1, 3}, {3, 2}, {2, 0}, {4, 5}, {5, 7}, {7, 6}, {6, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (auto &q : e) { dl->AddLine(pts[q[0]], pts[q[1]], IM_COL32(0, 0, 0, 160), topDown ? 4.0f : 3.6f); dl->AddLine(pts[q[0]], pts[q[1]], col, topDown ? 2.0f : 2.0f); }
    if (topDown) { float x, y; if (v.project(m.xform_point((b.mn + b.mx) * 0.5f), x, y)) dl->AddCircleFilled(ImVec2(x, y), 3.5f, col); }
  }
  (void)topDown;
}

bool Editor::handleDrag(render::View &v, bool topDown, bool hovered) {
  ImGuiIO &io = ImGui::GetIO();
  ImVec2 mp = io.MousePos;
  NodePath top = scene.world;
  // press: pick + arm drag
  if (hovered && ImGui::IsMouseClicked(0)) {
    double t0 = core::nowMs();
    render::PickHit h; h.eid = pickItem(v, mp.x, mp.y, h.point); h.hit = h.eid >= 0;
    if (topDown && h.eid < 0) {   // markers and tiny props: nearest item origin within 10 px
      float best = 100; int bi = -1;
      for (int i = 0; i < (int)doc.items.size(); i++) {
        if (doc.items[i].deleted || scene.itemNodes[i].is_empty() || scene.itemNodes[i].is_hidden()) continue;
        float x, y; if (!v.project(scene.itemNodes[i].get_pos(top), x, y)) continue;
        float d = (x - mp.x) * (x - mp.x) + (y - mp.y) * (y - mp.y); if (d < best) { best = d; bi = i; }
      }
      if (bi >= 0) h.eid = bi;
    }
    // prefer the outermost non-nested editable? keep innermost (what was clicked); Alt selects the parent
    if (h.eid >= 0 && io.KeyAlt) {
      NodePath p = scene.itemNodes[h.eid].get_parent().find_net_tag("eid");
      if (!p.is_empty()) h.eid = atoi(p.get_net_tag("eid").c_str());
    }
    if (h.eid >= 0) {
      bool isSel = std::find(selection.begin(), selection.end(), h.eid) != selection.end();
      if (io.KeyCtrl) { if (isSel) selection.erase(std::find(selection.begin(), selection.end(), h.eid)); else selection.push_back(h.eid); }
      else if (!isSel) selection = {h.eid};
      else { selection.erase(std::find(selection.begin(), selection.end(), h.eid)); selection.insert(selection.begin(), h.eid); }
      if (!selection.empty() && std::find(selection.begin(), selection.end(), h.eid) != selection.end()) {
        dragArmed_ = true; dragView_ = topDown ? 2 : 3; pressPos_ = mp;
        rotDrag_ = io.KeyShift; rotAccum_ = 0;
        dragZ_ = scene.itemNodes[h.eid].get_z(top);
        LPoint3 o; LVector3 d; dragLast_ = h.point;
        if (v.ray(mp.x, mp.y, o, d)) render::rayPlaneZ(o, d, dragZ_, dragLast_);
      }
    } else if (!io.KeyCtrl) selection.clear();
    core::setTiming("select (click -> selected)", core::nowMs() - t0);
  }
  int myView = topDown ? 2 : 3;
  if (dragArmed_ && dragView_ == myView && ImGui::IsMouseDown(0)) {
    float dx = mp.x - pressPos_.x, dy = mp.y - pressPos_.y;
    if (!dragging_ && dx * dx + dy * dy > 16) { dragging_ = true; beginEdit(); }
    if (dragging_ && io.MouseWheel != 0) {   // wheel while dragging: yaw by one step per notch
      float st = yawStep();
      for (size_t k = 0; k < editIds_.size(); k++) { auto &x = doc.items[editIds_[k]].xf; x.h += (io.MouseWheel > 0 ? st : -st); if (snapActive()) x.h = std::round(x.h / st) * st; scene.syncItem(doc, editIds_[k]); }
      wheelUsed_ = true;
    }
    if (dragging_ && rotDrag_) {   // Shift+drag: horizontal motion = yaw (0.5 deg/px), snapped
      double t0 = core::nowMs();
      float raw = dx * 0.5f * (topDown ? -1.f : -1.f);
      float st = snapActive() && snap.angle > 0 ? snap.angle : 0.f;
      float a = st > 0 ? std::round(raw / st) * st : raw;
      for (size_t k = 0; k < editIds_.size(); k++) { auto &x = doc.items[editIds_[k]].xf; x.h = editBefore_[k].h + a; scene.syncItem(doc, editIds_[k]); }
      rotAccum_ = a;
      core::setTiming("rotate (shift-drag update)", core::nowMs() - t0);
    } else if (dragging_) {
      double t0 = core::nowMs();
      LPoint3 o, hit; LVector3 d;
      if (v.ray(mp.x, mp.y, o, d) && render::rayPlaneZ(o, d, dragZ_, hit)) {
        LVector3 total = hit - dragLast_;
        // snap the primary object's new position, move the rest by the same delta
        int p0 = editIds_.empty() ? -1 : editIds_[0];
        if (p0 >= 0) {
          LPoint3 start = scene.itemNodes[p0].get_parent().get_relative_point(top, LPoint3(0)) ; (void)start;
          NodePath par = scene.itemNodes[p0].get_parent();
          LPoint3 s0 = top.get_relative_point(par, LPoint3(editBefore_[0].x, editBefore_[0].y, editBefore_[0].z));
          LPoint3 want = s0 + total; want[0] = snapv(want[0]); want[1] = snapv(want[1]);
          total = want - s0; total[2] = 0;
          for (size_t k = 0; k < editIds_.size(); k++) {
            int id = editIds_[k]; NodePath n = scene.itemNodes[id], pp = n.get_parent();
            LPoint3 s = top.get_relative_point(pp, LPoint3(editBefore_[k].x, editBefore_[k].y, editBefore_[k].z));
            n.set_pos(top, s + total); pushXformFromNode(id);
          }
        }
      }
      core::setTiming("move (drag update)", core::nowMs() - t0);
    }
  }
  if (dragArmed_ && dragView_ == myView && !ImGui::IsMouseDown(0)) {
    if (dragging_) endEdit(rotDrag_ ? "Rotate" : "Move");
    dragging_ = dragArmed_ = rotDrag_ = false;
  }
  return dragging_;
}

bool Editor::toggleWalker() {
  if (walker.active()) {
    walker.stop(); v3->target = walkerCameraTarget_; v3->dist = walkerCameraDist_;
    v3->yaw = walkerCameraYaw_; v3->pitch = walkerCameraPitch_; v3->apply(); return false;
  }
  if (!doc.root) return false;
  LPoint3 spawn = v3->target;
  float nearest = 1e30f;
  dna::walk(doc.root.get(), [&](const dna::XmlNode *node) {
    if (node->tag != "store_suit_point") return;
    LPoint3 candidate(strtof(node->attr("x").c_str(), nullptr), strtof(node->attr("y").c_str(), nullptr), strtof(node->attr("z").c_str(), nullptr));
    float distance = (candidate - v3->target).length_squared();
    if (distance < nearest) { nearest = distance; spawn = candidate; }
  });
  walkerCameraTarget_ = v3->target; walkerCameraDist_ = v3->dist;
  walkerCameraYaw_ = v3->yaw; walkerCameraPitch_ = v3->pitch;
  dragging_ = dragArmed_ = orbiting_ = false; pointerView_ = 0;
  bool started = walker.start(scene.world, scene.geom, spawn);
  if (started) { show3D_ = true; walker.camera(*v3); }
  return started;
}

void Editor::viewport3D() {
  if (!show3D_) { v3->setActive(false); return; }
  if (ImGui::GetIO().MouseWheel == 0) wheelUsed_ = false;
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0)); ImGui::PushStyleColor(ImGuiCol_DockingEmptyBg, ImVec4(0, 0, 0, 0));
  bool vis = ImGui::Begin("3D View", &show3D_, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground);
  ImGui::PopStyleVar(); ImGui::PopStyleColor(2);
  if (!show3D_ || (!vis && !forceViews)) { v3->setActive(false); ImGui::End(); return; }
  v3->setActive(true);
  ImVec2 p = ImGui::GetCursorScreenPos(), s = ImGui::GetContentRegionAvail();
  s.x = std::max(s.x, 8.f); s.y = std::max(s.y, 8.f);
  v3->setRect({p.x, p.y, s.x, s.y}, app_.width(), app_.height()); v3->apply();
  // no ImGui item here: ImGuizmo only activates when no ImGui item is hovered
  bool hovered = ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(p, ImVec2(p.x + s.x, p.y + s.y));
  if (hovered) hoveredView_ = "3D";
  ImGui::Dummy(s);
  ImGuiIO &io = ImGui::GetIO();
  if (walker.active()) {
    bool input = (hovered || ImGui::IsWindowFocused()) && !io.WantTextInput && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
    float forward = input ? float(ImGui::IsKeyDown(ImGuiKey_UpArrow)) - float(ImGui::IsKeyDown(ImGuiKey_DownArrow)) : 0;
    float turn = input ? float(ImGui::IsKeyDown(ImGuiKey_LeftArrow)) - float(ImGui::IsKeyDown(ImGuiKey_RightArrow)) : 0;
    walker.step(scene.geom, io.DeltaTime, forward, turn, input && io.KeyCtrl);
    walker.camera(*v3);
    ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + 12, p.y + 12), IM_COL32(255, 255, 255, 255), "Walk: arrow keys move / turn, Ctrl jumps. F5 returns to editing.");
    ImGui::End(); return;
  }
  bool down = ImGui::IsMouseDown(0) || ImGui::IsMouseDown(1) || ImGui::IsMouseDown(2);
  bool clicked = ImGui::IsMouseClicked(0) || ImGui::IsMouseClicked(1) || ImGui::IsMouseClicked(2);
  if (!down) { if (pointerView_ == 3) pointerView_ = 0; }
  else if (clicked && hovered && pointerView_ == 0) pointerView_ = 3;
  bool own = pointerView_ == 3;
  // gizmo first (it owns the mouse when hovered)
  bool gizmoHover = false, using_ = false;
  if (selection.size() >= 1 && !dragging_ && (own || pointerView_ == 0)) {
    int p0 = selection[0];
    bool was = editing_ && !dragging_;
    bool gChanged = false;
    const Box &bx = boxOf(p0);
    render::GizmoSnap gizmoSnap = snap; if (io.KeyAlt) gizmoSnap.on = false;
    using_ = render::gizmo(*v3, scene.itemNodes[p0], gizmoOp, gizmoSnap, &gizmoHover, &gChanged, (bx.mn + bx.mx) * 0.5f);
    if (gChanged && !using_ && was) pushXformFromNode(p0, true);   // last update arrives on the release frame
    if (using_) {
      if (!was) { selection = {p0}; beginEdit(); }
      double t0 = core::nowMs(); pushXformFromNode(p0, true); core::setTiming("move (gizmo update)", core::nowMs() - t0);
    } else if (was && !ImGui::IsMouseDown(0)) {
      endEdit(gizmoOp == render::GizmoOp::Rotate ? "Rotate" : gizmoOp == render::GizmoOp::Scale ? "Scale" : "Move");
    }
  }
  if (!gizmoHover && !using_) handleDrag(*v3, false, own || (hovered && pointerView_ == 0));
  // camera. A drag that started here keeps going after the cursor leaves the window.
  // The pivot is the camera itself. Yaw and pitch turn in place; the wheel steps a short way along the view.
  auto viewFwd = [&]() {
    float y = v3->yaw * float(M_PI) / 180.f, pth = v3->pitch * float(M_PI) / 180.f;
    return LVector3(std::sin(y) * std::cos(pth), std::cos(y) * std::cos(pth), std::sin(pth));
  };
  if ((own || (hovered && pointerView_ == 0)) && io.MouseWheel != 0 && !wheelUsed_) {
    if (consumeSelectionWheel(io)) {
      // Shift/Ctrl(/Alt)+wheel scaled or rotated the selection; do not zoom the camera.
    } else {
      // Fractional touchpad input stays fractional. Speed is independent of the
      // map's size or the distance retained from the last Frame All operation.
      // Fine zoom (Shift) only when nothing is selected (selection wheel owns Shift).
      float step = io.MouseWheel * wheelSpeed * (io.KeyShift ? 0.2f : 1.f);
      v3->target += viewFwd() * step;
    }
  }
  bool pan = own && (ImGui::IsMouseDown(2) || (ImGui::IsMouseDown(1) && io.KeyShift));
  if (own && ImGui::IsMouseDown(1) && !io.KeyShift) orbiting_ = true;
  if (!own || (!ImGui::IsMouseDown(1) && !ImGui::IsMouseDown(2))) orbiting_ = false;
  if (own && (orbiting_ || pan) && (io.MouseDelta.x || io.MouseDelta.y)) {
    float sx = invertCamera_ ? 1.f : -1.f;
    float sy = (invertCamera_ && invertCameraY_) ? 1.f : (invertCamera_ ? -1.f : (invertCameraY_ ? -1.f : 1.f));
    // Default invertCamera_=true: grab-the-world (view follows mouse). Off = classic orbit.
    if (pan) {
      LVector3 right = scene.world.get_relative_vector(v3->cam, LVector3(1, 0, 0)), up = scene.world.get_relative_vector(v3->cam, LVector3(0, 0, 1));
      float k = std::max(v3->dist, 12.f) * 0.0016f;
      v3->target -= right * io.MouseDelta.x * k * sx; v3->target += up * io.MouseDelta.y * k * sy;
    } else if (orbiting_) {
      LPoint3 camPos = v3->target - viewFwd() * v3->dist;
      v3->yaw -= io.MouseDelta.x * 0.3f * sx; v3->pitch -= io.MouseDelta.y * 0.3f * sy;
      v3->target = camPos + viewFwd() * v3->dist;
    }
  }
  if (hovered && ImGui::IsKeyDown(ImGuiKey_MouseRight) == false) {}
  v3->apply();
  drawSelection(*v3, false);
  // overlay
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const char *ops[] = {"Q drag", "W move", "E rotate", "R scale"};
  const char *snapHud = !snap.on ? "off" : (io.KeyAlt ? "bypass (Alt)" : "on");
  std::string hud = fmt("%s   |   snap %s (X)   |   %s   |   %.1f ms/frame", ops[(int)gizmoOp], snapHud,
                        light_.mode == render::LightMode::Game ? "Game light" : light_.mode == render::LightMode::Lit ? "Lit" : "Cog mood", lastFrameMs);
  dl->AddRectFilled(ImVec2(p.x + 8, p.y + 8), ImVec2(p.x + 16 + ImGui::CalcTextSize(hud.c_str()).x, p.y + 32), IM_COL32(15, 16, 20, 190), 5);
  dl->AddText(ImVec2(p.x + 12, p.y + 12), IM_COL32(220, 225, 235, 255), hud.c_str());
  ImGui::End();
}

void Editor::viewport2D() {
  if (!showMap_) { v2->setActive(false); return; }
  if (ImGui::GetIO().MouseWheel == 0) wheelUsed_ = false;
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0)); ImGui::PushStyleColor(ImGuiCol_DockingEmptyBg, ImVec4(0, 0, 0, 0));
  bool vis = ImGui::Begin("2D Map", &showMap_, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground);
  ImGui::PopStyleVar(); ImGui::PopStyleColor(2);
  if (!showMap_ || (!vis && !forceViews)) { v2->setActive(false); ImGui::End(); return; }
  v2->setActive(true);
  ImVec2 p = ImGui::GetCursorScreenPos(), s = ImGui::GetContentRegionAvail();
  s.x = std::max(s.x, 8.f); s.y = std::max(s.y, 8.f);
  v2->setRect({p.x, p.y, s.x, s.y}, app_.width(), app_.height());
  // no ImGui item here: ImGuizmo only activates when no ImGui item is hovered
  bool hovered = ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(p, ImVec2(p.x + s.x, p.y + s.y));
  if (hovered) hoveredView_ = "2D";
  ImGui::Dummy(s);
  ImGuiIO &io = ImGui::GetIO();
  bool down = ImGui::IsMouseDown(0) || ImGui::IsMouseDown(1) || ImGui::IsMouseDown(2);
  bool clicked = ImGui::IsMouseClicked(0) || ImGui::IsMouseClicked(1) || ImGui::IsMouseClicked(2);
  if (!down) { if (pointerView_ == 2) pointerView_ = 0; }
  else if (clicked && hovered && pointerView_ == 0) pointerView_ = 2;
  bool own = pointerView_ == 2;
  handleDrag(*v2, true, own || (hovered && pointerView_ == 0));
  if ((own || (hovered && pointerView_ == 0)) && io.MouseWheel != 0 && !wheelUsed_) {
    if (consumeSelectionWheel(io)) {
      // Selection transform consumed the wheel; leave the map zoom alone.
    } else {   // zoom around the cursor
      LPoint3 o, before, after; LVector3 d;
      bool a = v2->ray(io.MousePos.x, io.MousePos.y, o, d) && render::rayPlaneZ(o, d, 0, before);
      float mag = std::copysign(std::pow(std::fabs(io.MouseWheel), 0.65f), io.MouseWheel);
      v2->span *= std::pow(0.97f, mag);
      if (v2->span < 0.5f) v2->span = 0.5f;
      v2->apply();
      if (a && v2->ray(io.MousePos.x, io.MousePos.y, o, d) && render::rayPlaneZ(o, d, 0, after)) { v2->target += before - after; v2->target[2] = 0; }
    }
  }
  if (own && (ImGui::IsMouseDown(1) || ImGui::IsMouseDown(2)) && (io.MouseDelta.x || io.MouseDelta.y)) {
    float k = v2->span / std::max(1.f, std::min(s.x, s.y));
    float sx = invertCamera_ ? 1.f : -1.f;
    float sy = (invertCamera_ && invertCameraY_) ? 1.f : (invertCamera_ ? -1.f : (invertCameraY_ ? -1.f : 1.f));
    v2->target[0] -= io.MouseDelta.x * k * sx; v2->target[1] += io.MouseDelta.y * k * sy;
  }
  v2->apply();
  ImDrawList *dl = ImGui::GetWindowDrawList();
  // treasure spawns and other markers as dots, 3D view frustum centre as a cross
  NodePath top = scene.world;
  for (int i = 0; i < (int)doc.items.size(); i++) {
    auto &it = doc.items[i]; if (it.deleted || it.kind != "treasure_spawn" || scene.itemNodes[i].is_hidden()) continue;
    float x, y; if (v2->project(scene.itemNodes[i].get_pos(top), x, y)) dl->AddCircle(ImVec2(x, y), 4, IM_COL32(255, 215, 40, 230), 0, 2);
  }
  { float x, y; if (v2->project(v3->target, x, y)) { dl->AddLine(ImVec2(x - 7, y), ImVec2(x + 7, y), IM_COL32(255, 255, 255, 160)); dl->AddLine(ImVec2(x, y - 7), ImVec2(x, y + 7), IM_COL32(255, 255, 255, 160)); } }
  drawSelection(*v2, true);
  std::string hud = fmt("top-down  |  %.0f ft across  |  wheel zoom, right-drag pan", v2->span);
  dl->AddRectFilled(ImVec2(p.x + 8, p.y + 8), ImVec2(p.x + 16 + ImGui::CalcTextSize(hud.c_str()).x, p.y + 32), IM_COL32(15, 16, 20, 190), 5);
  dl->AddText(ImVec2(p.x + 12, p.y + 12), IM_COL32(220, 225, 235, 255), hud.c_str());
  ImGui::End();
}

void Editor::previewWindow() {
  if (!showPreview_) { pv->setActive(false); return; }
  ImGui::SetNextWindowSize(ImVec2(460, 560), ImGuiCond_FirstUseEver);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
  bool vis = ImGui::Begin("Prop Preview", &showPreview_, ImGuiWindowFlags_NoScrollbar);
  ImGui::PopStyleColor();
  if (!showPreview_ || !vis) { pv->setActive(false); ImGui::End(); return; }
  if (previewIdx_ < 0) { ImGui::TextDisabled("Pick a prop in the palette."); ImGui::End(); return; }
  auto &e = catalog[previewIdx_];
  ImGui::Text("%s", e.code.c_str());
  ImGui::TextDisabled("root %s  |  %s", e.root.c_str(), fs::path(e.storageRel).filename().string().c_str());
  ImGui::TextDisabled("model %s%s%s", e.model.c_str(), e.node.empty() ? "" : "  node ", e.node.c_str());
  ImGui::Text("size  %.2f x %.2f x %.2f ft   |   %d tris", previewSize_[0], previewSize_[1], previewSize_[2], previewTris_);
  bool can = !doc.path.empty();
  if (!can) ImGui::BeginDisabled();
  if (ImGui::Button("  Place in scene  ")) placePreviewed();
  if (!can) ImGui::EndDisabled();
  ImGui::SameLine(); if (ImGui::Button("Reset view")) { pvYaw_ = 35; pvPitch_ = 20; pvZoom_ = 1; }
  ImGui::SameLine(); ImGui::TextDisabled(loadedCodes_.count(e.code) ? "in this file's storage" : "not in this file's storage");
  ImVec2 p = ImGui::GetCursorScreenPos(), s = ImGui::GetContentRegionAvail(); s.x = std::max(s.x, 8.f); s.y = std::max(s.y, 8.f);
  ImGui::InvisibleButton("##pv", s, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
  ImGuiIO &io = ImGui::GetIO();
  if (ImGui::IsItemActive()) { pvYaw_ -= io.MouseDelta.x * 0.5f; pvPitch_ = std::clamp(pvPitch_ + io.MouseDelta.y * 0.4f, -85.f, 85.f); }
  if (ImGui::IsItemHovered() && io.MouseWheel) pvZoom_ = std::clamp(pvZoom_ * std::pow(0.88f, io.MouseWheel), 0.1f, 10.f);
  // frame the model: fit radius in view
  float r = std::max(previewSize_.length() / 2, 0.5f);
  pv->setActive(true); pv->setRect({p.x, p.y, s.x, s.y}, app_.width(), app_.height());
  pv->yaw = pvYaw_; pv->pitch = -pvPitch_; pv->dist = r / std::tan(30 * M_PI / 180) * 1.15f * pvZoom_; pv->apply();
  ImGui::End();
}

void Editor::visgroupsPanel() {
  if (!showVisgroups_) return;
  ImGui::Begin("Visgroups", &showVisgroups_);
  if (!showVisgroups_) { ImGui::End(); return; }
  if (doc.visgroups.empty()) ImGui::TextDisabled("No visgroups in this file.");
  if (ImGui::SmallButton("Show all")) for (auto &[k, v] : vgVisible_) { v = true; scene.setVisgroupVisible(k, true); }
  ImGui::SameLine(); if (ImGui::SmallButton("Hide all")) for (auto &[k, v] : vgVisible_) { v = false; scene.setVisgroupVisible(k, false); }
  ImGui::SameLine(); if (ImGui::SmallButton("No filter")) out_.visgroup.clear();
  ImGui::BeginChild("vgl");
  for (auto &z : doc.visgroups) {
    ImGui::PushID(z.c_str());
    bool v = vgVisible_[z];
    if (ImGui::Checkbox("##v", &v)) { vgVisible_[z] = v; scene.setVisgroupVisible(z, v); }
    ImGui::SameLine();
    int n = 0; for (auto &it : doc.items) n += (!it.deleted && it.visgroup == z);
    if (ImGui::Selectable(fmt("%s   (%d)", z.c_str(), n).c_str(), out_.visgroup == z)) out_.visgroup = out_.visgroup == z ? "" : z;
    ImGui::PopID();
  }
  ImGui::EndChild();
  ImGui::End();
}

void Editor::statsPanel() {
  if (!showTimings_) return;
  ImGui::Begin("Timings", &showTimings_);
  if (!showTimings_) { ImGui::End(); return; }
  ImGui::Text("frame %.2f ms (%.0f fps)", lastFrameMs, lastFrameMs > 0 ? 1000.0 / lastFrameMs : 0.0);
  ImGui::Separator();
  for (auto &[k, v] : core::timings()) if (k.rfind("load model", 0) != 0) ImGui::Text("%-34s %9.2f ms", k.c_str(), v);
  ImGui::End();
}


bool Editor::isAppearanceOverride() const {
  return doc.text.find("appearance_override") != std::string::npos;
}

bool Editor::injectOverrideOp(const std::string &tag, const std::string &code,
                              const std::string &extraAttrs) {
  if (code.empty() || doc.text.empty()) return false;
  size_t pos = doc.text.rfind("</group>");
  if (pos == std::string::npos) {
    // No group yet: insert before </scene>
    pos = doc.text.rfind("</scene>");
    if (pos == std::string::npos) return false;
    std::string block = "\t<group name=\"override\" gate=\"" + std::string(ovGate_) + "\"";
    if (std::string(ovGate_) == "holiday")
      block += " holidays=\"" + std::string(ovHolidays_) + "\"";
    block += ">\n\t\t<" + tag + " code=\"" + code + "\"";
    if (!extraAttrs.empty()) block += " " + extraAttrs;
    block += "/>\n\t</group>\n";
    doc.text.insert(pos, block);
  } else {
    std::string line = "\t\t<" + tag + " code=\"" + code + "\"";
    if (!extraAttrs.empty()) line += " " + extraAttrs;
    line += "/>\n";
    doc.text.insert(pos, line);
  }
  doc.textDirty = true;
  overrideDirty_ = true;
  core::info("AODNA: queued <" + tag + " code=\"" + code + "\"> (save to write)");
  return true;
}

bool Editor::setOverrideGate(const std::string &gate, const std::string &holidays) {
  if (doc.text.empty()) return false;
  std::regex re(R"(<group\b[^>]*>)");
  std::smatch m;
  if (!std::regex_search(doc.text, m, re)) return false;
  std::string tag = m.str(0);
  auto setAttr = [](std::string t, const std::string &k, const std::string &v) {
    std::regex re2("(\\b" + k + "\\s*=\\s*)(\"[^\"]*\"|'[^']*')");
    std::smatch mm;
    if (std::regex_search(t, mm, re2)) {
      return t.substr(0, mm.position(0)) + mm.str(1) + "\"" + v + "\"" +
             t.substr(mm.position(0) + mm.length(0));
    }
    size_t gt = t.rfind('>');
    if (gt == std::string::npos) return t;
    return t.substr(0, gt) + " " + k + "=\"" + v + "\"" + t.substr(gt);
  };
  tag = setAttr(tag, "gate", gate);
  if (gate == "holiday") tag = setAttr(tag, "holidays", holidays);
  doc.text.replace((size_t)m.position(0), (size_t)m.length(0), tag);
  doc.textDirty = true;
  overrideDirty_ = true;
  core::info("AODNA: gate=" + gate + " holidays=" + holidays);
  return true;
}

bool Editor::newAppearanceOverride() {
  // Opt-in skeleton under phase_4/dna/overrides/ — does not touch existing DNA.
  fs::path dir = fs::path(resRoot) / "phase_4" / "dna" / "overrides";
  std::error_code ec;
  fs::create_directories(dir, ec);
  if (ec) { core::error("cannot create overrides dir: " + ec.message()); return false; }
  std::string name = "new_override.xml";
  fs::path out = dir / name;
  int n = 1;
  while (fs::exists(out)) {
    name = "new_override_" + std::to_string(n++) + ".xml";
    out = dir / name;
  }
  const char *skel =
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
      "<!-- Appearance Override DNA (opt-in). appearance_override=1.\n"
      "     Place add-props from the Palette. Use the Appearance Override panel\n"
      "     for holiday gate, hide, replace, and recolor. Normal DNA is unchanged. -->\n"
      "<scene zone=\"2000\" appearance_override=\"1\">\n"
      "\t<group name=\"override\" gate=\"holiday\" holidays=\"HALLOWEEN_PROPS,SPOOKY_PROPS\">\n"
      "\t</group>\n"
      "</scene>\n";
  {
    std::ofstream f(out);
    if (!f) { core::error("cannot write " + out.string()); return false; }
    f << skel;
  }
  core::info("created " + out.string());
  return open(out.string());
}


void Editor::loadOverrideEffectsFromDoc() {
  ovFxLoaded_ = true;
  // Defaults match HalloweenGazeboFog._DEFAULTS when XML has no effect yet.
  auto grab = [&](const char *type, const char *key, float &dst) {
    std::string needle = std::string("type=\"") + type + "\"";
    size_t p = doc.text.find(needle);
    if (p == std::string::npos) return;
    size_t lineStart = doc.text.rfind('<', p);
    size_t lineEnd = doc.text.find('>', p);
    if (lineStart == std::string::npos || lineEnd == std::string::npos) return;
    std::string line = doc.text.substr(lineStart, lineEnd - lineStart);
    std::string k = std::string(key) + "=\"";
    size_t a = line.find(k);
    if (a == std::string::npos) return;
    a += k.size();
    size_t b = line.find('"', a);
    if (b == std::string::npos) return;
    try { dst = std::stof(line.substr(a, b - a)); } catch (...) {}
  };
  grab("ground_fog", "mistOpacity", ovFxMistOpacity_);
  grab("ground_fog", "mistDensity", ovFxMistDensity_);
  grab("ground_fog", "mistHeight", ovFxMistHeight_);
  grab("ground_fog", "mistSpeed", ovFxMistSpeed_);
  grab("ground_fog", "mistHue", ovFxMistHue_);
  grab("ground_fog", "mistRadius", ovFxMistRadius_);
  grab("lantern_glow", "lanternGlowSize", ovFxLanternSize_);
  grab("lantern_glow", "lanternGlowIntensity", ovFxLanternIntensity_);
  grab("lantern_glow", "flashSpeed", ovFxFlashSpeed_);
  grab("lantern_glow", "faceGlow", ovFxFaceGlow_);
  grab("lantern_glow", "flickerAmount", ovFxFlickerAmount_);
  grab("lantern_glow", "flickerSpeed", ovFxFlickerSpeed_);
}

bool Editor::applyOverrideEffects() {
  if (!isAppearanceOverride() || doc.text.empty()) return false;
  auto fmtf = [](float v) {
    char buf[32]; snprintf(buf, sizeof buf, "%.4g", (double)v); return std::string(buf);
  };
  std::string fog = "<effect type=\"ground_fog\" target=\"gazebo\" mistOpacity=\"" +
      fmtf(ovFxMistOpacity_) + "\" mistDensity=\"" + fmtf(ovFxMistDensity_) +
      "\" mistHeight=\"" + fmtf(ovFxMistHeight_) + "\" mistSpeed=\"" + fmtf(ovFxMistSpeed_) +
      "\" mistHue=\"" + fmtf(ovFxMistHue_) + "\" mistRadius=\"" + fmtf(ovFxMistRadius_) + "\"/>";
  std::string glow = "<effect type=\"lantern_glow\" target=\"gazebo\" lanternGlowSize=\"" +
      fmtf(ovFxLanternSize_) + "\" lanternGlowIntensity=\"" + fmtf(ovFxLanternIntensity_) +
      "\" flashSpeed=\"" + fmtf(ovFxFlashSpeed_) + "\" faceGlow=\"" + fmtf(ovFxFaceGlow_) +
      "\" flickerAmount=\"" + fmtf(ovFxFlickerAmount_) + "\" flickerSpeed=\"" + fmtf(ovFxFlickerSpeed_) + "\"/>";
  auto repl = [&](const char *type, const std::string &el) {
    std::string needle = std::string("type=\"") + type + "\"";
    size_t p = doc.text.find(needle);
    if (p != std::string::npos) {
      size_t a = doc.text.rfind('<', p);
      size_t b = doc.text.find('>', p);
      if (a != std::string::npos && b != std::string::npos) {
        // include optional trailing newline
        size_t end = b + 1;
        if (end < doc.text.size() && doc.text[end] == '\n') end++;
        size_t ls = a;
        while (ls > 0 && (doc.text[ls - 1] == ' ' || doc.text[ls - 1] == '\t')) ls--;
        doc.text.replace(ls, end - ls, std::string("\t\t") + el + "\n");
        return;
      }
    }
    size_t g = doc.text.rfind("</group>");
    if (g != std::string::npos)
      doc.text.insert(g, std::string("\t\t") + el + "\n");
  };
  repl("ground_fog", fog);
  repl("lantern_glow", glow);
  doc.textDirty = true;
  overrideDirty_ = true;
  core::info("AODNA effects updated (Ctrl+S to save)");
  return true;
}


void Editor::setPreviewEffects(bool on) {
  ovPreviewEffects_ = on;
  if (!on) clearEffectsPreview();
}

void Editor::setAnimateBats(bool on) {
  ovAnimateBats_ = on;
  if (!on) {
    updateBatsPreview();  // drops the wing-texture override
    for (int i = 0; i < (int)doc.items.size() && i < (int)scene.itemNodes.size(); i++)
      if (doc.items[i].node && doc.items[i].node->attr("aodna_role") == "bat") scene.syncItem(doc, i);
  }
}

void Editor::clearEffectsPreview() {
  // Restore lantern/pumpkin colour; drop face decals, labels, halos, fog. Never touches XML.
  for (int i = 0; i < (int)doc.items.size() && i < (int)scene.itemNodes.size(); i++) {
    NodePath np = scene.itemNodes[i];
    if (np.is_empty()) continue;
    NodePath dec = np.find("**/fx_face_glow");
    if (!dec.is_empty()) dec.remove_node();
    NodePath fg = np.find("**/face_glow");
    if (!fg.is_empty()) fg.clear_color_scale();
    auto &it = doc.items[i];
    if (it.node && (it.node->attr("aodna_role") == "lantern" || !it.node->attr("lantern_index").empty()))
      np.clear_color_scale();
  }
  if (!fxPreviewRoot_.is_empty()) { fxPreviewRoot_.remove_node(); fxPreviewRoot_ = NodePath(); }
  fxPreviewBuilt_ = false;
}

// Face-only glow layer for a jack-o'-lantern, matching the game's HalloweenFaceGlow:
// TTEX pumpkins ship a "face_glow" child; the legacy hanging lantern gets a cached decal
// (its own geometry re-textured with the carved-face alpha mask, added on top).
NodePath Editor::faceLayerFor(NodePath np, bool &decal) {
  decal = false;
  if (np.is_empty()) return NodePath();
  NodePath fg = np.find("**/face_glow");
  if (!fg.is_empty()) { fg.set_light_off(1); return fg; }
  NodePath dec = np.find("**/fx_face_glow");
  if (!dec.is_empty()) { decal = true; return dec; }
  if (!fxFaceMaskTex_)
    fxFaceMaskTex_ = assets::ModelCache::get().texture("phase_4/models/props/ttex_t_prop_halloween_hanging_lantern_faceglow.png");
  if (!fxFaceMaskTex_) return NodePath();
  NodePathCollection geoms = np.find_all_matches("**/+GeomNode");
  if (geoms.get_num_paths() == 0) return NodePath();
  dec = np.attach_new_node("fx_face_glow");
  for (int k = 0; k < geoms.get_num_paths(); k++) {
    NodePath g = geoms.get_path(k);
    if (dec.is_ancestor_of(g)) continue;
    NodePath cpy = g.copy_to(dec);
    cpy.set_transform(g.get_transform(np));
  }
  dec.set_texture(fxFaceMaskTex_, 100);
  dec.set_material_off(100);
  dec.set_light_off(100);
  dec.set_transparency(TransparencyAttrib::M_alpha);
  dec.set_attrib(ColorBlendAttrib::make(ColorBlendAttrib::M_add, ColorBlendAttrib::O_incoming_alpha,
                                        ColorBlendAttrib::O_one), 100);
  dec.set_depth_write(false);
  dec.set_attrib(DepthOffsetAttrib::make(2));
  dec.set_bin("fixed", 30);
  dec.set_tag("fx_preview", "1");
  decal = true;
  return dec;
}

PT(Texture) Editor::ensureHaloTex() {
  if (fxHaloTex_) return fxHaloTex_;
  // Soft orange radial glow (procedural, 64x64).
  const int N = 64;
  PT(Texture) tex = new Texture("fxHalo");
  tex->setup_2d_texture(N, N, Texture::T_unsigned_byte, Texture::F_rgba);
  PTA_uchar img = tex->modify_ram_image();
  for (int y = 0; y < N; y++) for (int x = 0; x < N; x++) {
    float u = (x + 0.5f) / N * 2.f - 1.f, v = (y + 0.5f) / N * 2.f - 1.f;
    float d = std::sqrt(u * u + v * v);
    float a = std::clamp(1.f - d, 0.f, 1.f); a = a * a * (3.f - 2.f * a);
    int i = (y * N + x) * 4;
    img[i] = 255; img[i+1] = (unsigned char)(180); img[i+2] = 60; img[i+3] = (unsigned char)(a * 255);
  }
  tex->set_wrap_u(SamplerState::WM_clamp); tex->set_wrap_v(SamplerState::WM_clamp);
  fxHaloTex_ = tex; return fxHaloTex_;
}

PT(Texture) Editor::ensureCloudTex() {
  if (fxCloudTex_) return fxCloudTex_;
  const int N = 64;
  PT(Texture) tex = new Texture("fxCloud");
  tex->setup_2d_texture(N, N, Texture::T_unsigned_byte, Texture::F_rgba);
  PTA_uchar img = tex->modify_ram_image();
  for (int y = 0; y < N; y++) for (int x = 0; x < N; x++) {
    float u = (x + 0.5f) / N * 2.f - 1.f, v = (y + 0.5f) / N * 2.f - 1.f;
    float d = std::sqrt(u * u + v * v * 0.6f);
    float a = std::clamp(1.f - d, 0.f, 1.f); a = a * a;
    int i = (y * N + x) * 4;
    // soft lavender
    img[i] = 200; img[i+1] = 180; img[i+2] = 230; img[i+3] = (unsigned char)(a * 200);
  }
  tex->set_wrap_u(SamplerState::WM_clamp); tex->set_wrap_v(SamplerState::WM_clamp);
  fxCloudTex_ = tex; return fxCloudTex_;
}

void Editor::updateBatsPreview() {
  // Animate bats = the game's two-card wing swap (wings_up <-> wings_down texture), each bat
  // on its own phase. It never moves the node, so gizmo edits always land on the rest pose
  // (it.xf). Off (default): drop the texture override so the authored PNG shows. No XML.
  if (!ovAnimateBats_ && !fxBatsSwapped_) return;
  double now = ClockObject::get_global_clock()->get_frame_time();
  const double HALF = 0.11;  // matches game BAT_FLAP_HALF
  if (ovAnimateBats_ && (!fxBatUpTex_ || !fxBatDownTex_)) {
    fxBatUpTex_ = assets::ModelCache::get().texture("phase_4/models/props/tt_t_ara_prop_halloween_bat_wings_up.png");
    fxBatDownTex_ = assets::ModelCache::get().texture("phase_4/models/props/tt_t_ara_prop_halloween_bat_wings_down.png");
  }
  int n = 0;
  for (int i = 0; i < (int)doc.items.size() && i < (int)scene.itemNodes.size(); i++) {
    auto &it = doc.items[i];
    if (!it.node) continue;
    if (it.node->attr("aodna_role") != "bat") continue;
    NodePath np = scene.itemNodes[i];
    if (np.is_empty()) continue;
    if (!ovAnimateBats_ || !fxBatUpTex_ || !fxBatDownTex_) { np.clear_texture(); continue; }
    double phase = std::fmod(n * 0.37, 2 * HALF);
    bool up = ((long long)std::floor((now + phase) / HALF)) % 2 == 0;
    np.set_texture(up ? fxBatUpTex_ : fxBatDownTex_, 100);
    n++;
  }
  fxBatsSwapped_ = ovAnimateBats_;
}

void Editor::updateEffectsPreview() {
  if (!ovPreviewEffects_ || !isAppearanceOverride()) return;
  // Match HalloweenGazeboLanterns: odds (index 0,2,4 → labels 1,3,5) then evens.
  float speed = std::max(0.25f, ovFxFlashSpeed_);
  float fade = 0.90f / speed, hold = 0.70f / speed;
  float half = fade + hold, cycle = 2.f * half;
  double now = ClockObject::get_global_clock()->get_frame_time();
  float t = (float)std::fmod(now, (double)cycle);
  bool oddsBright = t < half;
  float phase = oddsBright ? t : (t - half);
  float u = phase < fade ? (phase / fade) : 1.f;  // 0→1 during fade, hold at 1
  // ease in-out
  float e = u * u * (3.f - 2.f * u);

  struct Lamp { int idx; int item; NodePath np; bool lantern; };
  std::vector<Lamp> lamps;
  int pumpkinN = 0;
  for (int i = 0; i < (int)doc.items.size() && i < (int)scene.itemNodes.size(); i++) {
    auto &it = doc.items[i];
    if (!it.node || it.deleted) continue;
    NodePath np = scene.itemNodes[i];
    if (np.is_empty()) continue;
    std::string li = it.node->attr("lantern_index");
    bool lantern = it.node->attr("aodna_role") == "lantern" || !li.empty();
    if (!lantern) {
      // TTEX pumpkins with a carved face_glow layer flash with the lanterns too
      if (np.find("**/face_glow").is_empty()) continue;
      int idx = li.empty() ? pumpkinN++ : atoi(li.c_str());
      lamps.push_back({idx, i, np, false});
      continue;
    }
    int idx = li.empty() ? (int)lamps.size() : atoi(li.c_str());
    lamps.push_back({idx, i, np, true});
  }
  std::sort(lamps.begin(), lamps.end(), [](const Lamp &a, const Lamp &b) {
    return a.lantern != b.lantern ? a.lantern : a.idx < b.idx; });

  auto lerpC = [](const LColor &a, const LColor &b, float f) {
    return LColor(a[0] + (b[0] - a[0]) * f, a[1] + (b[1] - a[1]) * f,
                  a[2] + (b[2] - a[2]) * f, a[3] + (b[3] - a[3]) * f);
  };

  if (fxPreviewRoot_.is_empty()) fxPreviewRoot_ = scene.world.attach_new_node("fxPreviewLabels");
  fxPreviewRoot_.get_children().detach();

  PT(Texture) haloTex = ensureHaloTex();
  float haloSize = ovFxLanternSize_;
  float haloInt = ovFxLanternIntensity_;
  const float g = std::clamp(ovFxFaceGlow_, 0.f, 2.f);
  const float amt = std::clamp(ovFxFlickerAmount_, 0.f, 1.f), fsp = std::max(0.05f, ovFxFlickerSpeed_);

  for (auto &L : lamps) {
    bool odd = (L.idx % 2) == 0;  // game: lanternIndex%2==0 → odds group (labels 1,3,5)
    float f = (odd == oddsBright) ? e : (1.f - e);   // 0 = dim, 1 = lit
    // candle flicker (same sum of sines as HalloweenFaceGlow.flickerValue)
    float seed = (float)(L.item % 7) * 1.618f;
    float tt = (float)now;
    float v = std::sin(tt * 7.3f * fsp + seed) * 0.45f + std::sin(tt * 13.1f * fsp + seed * 2.3f) * 0.35f +
              std::sin(tt * 23.7f * fsp + seed * 4.1f) * 0.20f;
    float flick = 1.f - amt * (0.5f + 0.5f * v);
    if (L.lantern) L.np.set_color_scale(0.82f, 0.78f, 0.74f, 1.f);   // body steady; only the face glows
    bool decal = false;
    NodePath face = faceLayerFor(L.np, decal);
    if (!face.is_empty()) {
      if (decal) {
        const float k = 0.6f + 0.5f * g;   // matches HalloweenFaceGlow.onOffColors
        LColor on(1.f * k, 0.80f * k, 0.38f * k, std::min(1.f, 0.95f * g)), off(1.f, 0.7f, 0.4f, 0.04f * g);
        LColor c = lerpC(off, on, f); c[3] *= flick;
        face.set_color_scale(c);
      } else {
        LColor on(1.f + 0.90f * g, 1.f + 0.60f * g, 0.50f + 0.10f * g, 1.f), off(0.48f, 0.34f, 0.20f, 1.f);
        LColor c = lerpC(off, on, f);
        face.set_color_scale(c[0] * flick, c[1] * (0.94f * flick + 0.06f), c[2] * (0.85f * flick + 0.15f), 1.f);
      }
    }
    if (!L.lantern) continue;   // pumpkins: no label / halo

    // 1-based label
    PT(TextNode) tn = new TextNode("lanternIdx");
    tn->set_text(std::to_string(L.idx + 1));
    tn->set_align(TextNode::A_center);
    tn->set_text_color(1, 0.95f, 0.4f, 1);
    tn->set_shadow(0.07f, 0.07f); tn->set_shadow_color(0, 0, 0, 1);   // readable on the roof / sky
    NodePath lab = fxPreviewRoot_.attach_new_node(tn);
    lab.set_scale(1.1f);
    lab.set_pos(L.np.get_pos(scene.world) + LVector3(0, 0, 1.8f));
    lab.set_effect(BillboardEffect::make_point_eye());
    lab.set_bin("fixed", 60);
    lab.set_depth_test(false); lab.set_depth_write(false);

    // Soft halo card
    CardMaker cm("halo");
    cm.set_frame(-haloSize, haloSize, -haloSize, haloSize);
    NodePath halo = fxPreviewRoot_.attach_new_node(cm.generate());
    halo.set_texture(haloTex);
    halo.set_transparency(TransparencyAttrib::M_alpha);
    float ha = (odd == oddsBright ? e : (1.f - e)) * haloInt;
    halo.set_color_scale(1.f, 0.7f, 0.25f, ha);
    halo.set_pos(L.np.get_pos(scene.world));
    halo.set_effect(BillboardEffect::make_point_eye());
    halo.set_bin("transparent", 20);
    halo.set_depth_write(false);
  }

  // Simple ground-fog wisps (a few billboard cards) — live only.
  PT(Texture) cloud = ensureCloudTex();
  float rad = ovFxMistRadius_, opac = ovFxMistOpacity_, hgt = ovFxMistHeight_;
  for (int k = 0; k < 8; k++) {
    float ang = (float)k / 8.f * 6.28318f + (float)now * ovFxMistSpeed_ * 0.4f;
    float r = rad * (0.55f + 0.35f * std::sin(ang * 1.7f + k));
    CardMaker cm("fog");
    float s = 2.2f + 0.6f * std::sin((float)now + k);
    cm.set_frame(-s, s, -s * 0.45f, s * 0.45f);
    NodePath fog = fxPreviewRoot_.attach_new_node(cm.generate());
    fog.set_texture(cloud);
    fog.set_transparency(TransparencyAttrib::M_alpha);
    fog.set_color_scale(0.78f, 0.70f, 0.90f, opac * (0.6f + 0.4f * std::sin((float)now * 0.7f + k)));
    fog.set_pos(std::cos(ang) * r, std::sin(ang) * r, hgt * 0.5f);
    fog.set_effect(BillboardEffect::make_point_eye());
    fog.set_bin("transparent", 15);
    fog.set_depth_write(false);
  }
  fxPreviewBuilt_ = true;
}


void Editor::overridePanel() {
  if (!isAppearanceOverride()) return;  // opt-in: never show on normal DNA
  if (getenv("DNAED_SNAP_HIDE_OVERRIDE")) return;   // snapshot hook: keep the 3D view clear
  ImGui::Begin("Appearance Override");
  ImGui::TextWrapped(
      "Opt-in holiday/appearance overlay. Normal DNA files never see this panel.\n"
      "Add props: place from the Palette (parent_code attaches to a zone model).\n"
      "Hide / Replace / Recolor: select a prop or type its DNA code.");
  ImGui::Separator();
  ImGui::SetNextItemWidth(120);
  ImGui::InputText("gate", ovGate_, sizeof ovGate_);
  ImGui::SetNextItemWidth(280);
  ImGui::InputText("holidays", ovHolidays_, sizeof ovHolidays_);
  if (ImGui::Button("Apply gate")) setOverrideGate(ovGate_, ovHolidays_);
  ImGui::Separator();
  ImGui::TextWrapped("ATTACH-TO-MODEL: new Palette places get parent_code below. "
                     "Pos/hpr/scale are local to that DNA prop (e.g. gazebo).");
  ImGui::SetNextItemWidth(180);
  ImGui::InputText("parent_code", ovParentCode_, sizeof ovParentCode_);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(140);
  ImGui::InputText("parent_node", ovParentNode_, sizeof ovParentNode_);
  if (ImGui::Button("Attach selected to parent_code") && !selection.empty() && ovParentCode_[0]) {
    for (int i : selection) {
      if (i < 0 || i >= (int)doc.items.size()) continue;
      auto &it = doc.items[i];
      if (it.node) {
        doc.setNodeAttribute(it.node, "parent_code", ovParentCode_);
        if (ovParentNode_[0])
          doc.setNodeAttribute(it.node, "parent_node", ovParentNode_);
        else
          doc.setNodeAttribute(it.node, "parent_node", "");
      } else if (!it.templateXml.empty()) {
        // Unsaved add: rewrite opening tag attrs.
        auto amp = it.templateXml.find('>');
        if (amp != std::string::npos) {
          std::string open = it.templateXml.substr(0, amp);
          auto strip = [](std::string &s, const char *key) {
            auto p = s.find(key);
            if (p == std::string::npos) return;
            auto q = s.find('"', p); if (q == std::string::npos) return;
            auto r = s.find('"', q + 1); if (r == std::string::npos) return;
            s.erase(p, r + 1 - p);
          };
          strip(open, "parent_code=");
          strip(open, "parent_node=");
          open += " parent_code=\"" + std::string(ovParentCode_) + "\"";
          if (ovParentNode_[0])
            open += " parent_node=\"" + std::string(ovParentNode_) + "\"";
          it.templateXml = open + it.templateXml.substr(amp);
        }
      }
      overrideDirty_ = true;
    }
    // Re-parse so attrs stick for next save of sourced items
    if (doc.textDirty || overrideDirty_) { /* save will keep template attrs */ }
    core::info("set parent_code=" + std::string(ovParentCode_) + " on selection");
  }
  ImGui::Separator();
  ImGui::SetNextItemWidth(220);
  ImGui::InputText("code", ovCode_, sizeof ovCode_);
if (ImGui::Button("Use selected code") && !selection.empty()) {
    int i = selection[0];
    if (i >= 0 && i < (int)doc.items.size() && !doc.items[i].code.empty())
      snprintf(ovCode_, sizeof ovCode_, "%s", doc.items[i].code.c_str());
  }
  auto codes = [&]() {
    std::vector<std::string> out;
    if (ovCode_[0]) out.push_back(ovCode_);
    else for (int i : selection)
      if (i >= 0 && i < (int)doc.items.size() && !doc.items[i].code.empty())
        out.push_back(doc.items[i].code);
    return out;
  };
  auto list = codes();
  bool has = !list.empty();
  if (ImGui::Button("Hide", ImVec2(90, 0)) && has) {
    for (auto &c : list) injectOverrideOp("hide", c);
  }
  ImGui::SameLine();
  if (ImGui::Button("Replace", ImVec2(90, 0)) && has) {
    for (auto &c : list) injectOverrideOp("replace", c);
  }
  ImGui::SameLine();
  ImGui::SetNextItemWidth(110);
  ImGui::InputText("##recolor", ovRecolor_, sizeof ovRecolor_);
  ImGui::SameLine();
  if (ImGui::Button("Recolor", ImVec2(90, 0)) && has) {
    std::string extra = "find=\"**/=DNACode=" + list[0] + "\" color_scale=\"" +
                        std::string(ovRecolor_) + "\"";
    // recolor uses find= not only code= — still stamp code for readability
    for (auto &c : list) {
      std::string ex = "find=\"**/=DNACode=" + c + "\" color_scale=\"" +
                       std::string(ovRecolor_) + "\"";
      injectOverrideOp("recolor", c, ex);
    }
  }
  ImGui::Separator();
  if (!ovFxLoaded_) loadOverrideEffectsFromDoc();
  ImGui::TextWrapped("Effects / lighting (written as <effect> — same keys as ~lightTuner Gazebo). "
                     "Game uses these when want-appearance-override is on.");
  ImGui::TextDisabled("ground_fog");
  ImGui::SetNextItemWidth(120); ImGui::DragFloat("mistOpacity", &ovFxMistOpacity_, 0.01f, 0.02f, 0.72f, "%.3f");
  ImGui::SameLine(); ImGui::SetNextItemWidth(120); ImGui::DragFloat("mistDensity", &ovFxMistDensity_, 0.01f, 0.05f, 1.5f, "%.3f");
  ImGui::SetNextItemWidth(120); ImGui::DragFloat("mistHeight", &ovFxMistHeight_, 0.01f, 0.2f, 2.5f, "%.3f");
  ImGui::SameLine(); ImGui::SetNextItemWidth(120); ImGui::DragFloat("mistSpeed", &ovFxMistSpeed_, 0.01f, 0.15f, 2.0f, "%.3f");
  ImGui::SetNextItemWidth(120); ImGui::DragFloat("mistHue", &ovFxMistHue_, 0.01f, 0.0f, 1.0f, "%.3f");
  ImGui::SameLine(); ImGui::SetNextItemWidth(120); ImGui::DragFloat("mistRadius", &ovFxMistRadius_, 0.1f, 3.0f, 30.0f, "%.2f");
  ImGui::TextDisabled("lantern_glow");
  ImGui::SetNextItemWidth(120); ImGui::DragFloat("lanternGlowSize", &ovFxLanternSize_, 0.01f, 0.9f, 2.2f, "%.3f");
  ImGui::SameLine(); ImGui::SetNextItemWidth(120); ImGui::DragFloat("lanternGlowIntensity", &ovFxLanternIntensity_, 0.01f, 0.05f, 1.0f, "%.3f");
  ImGui::SetNextItemWidth(120); ImGui::DragFloat("flashSpeed", &ovFxFlashSpeed_, 0.01f, 0.25f, 2.5f, "%.3f");
  ImGui::SameLine(); ImGui::SetNextItemWidth(120); ImGui::DragFloat("faceGlow", &ovFxFaceGlow_, 0.01f, 0.0f, 2.0f, "%.3f");
  ImGui::SetNextItemWidth(120); ImGui::DragFloat("flickerAmount", &ovFxFlickerAmount_, 0.01f, 0.0f, 1.0f, "%.3f");
  ImGui::SameLine(); ImGui::SetNextItemWidth(120); ImGui::DragFloat("flickerSpeed", &ovFxFlickerSpeed_, 0.01f, 0.2f, 4.0f, "%.3f");
  {
    bool prev = ovPreviewEffects_;
    if (ImGui::Checkbox("Preview effects", &prev)) setPreviewEffects(prev);
    ImGui::SameLine();
    ImGui::TextDisabled("(face-only odd/even flash + candle flicker, 1-6 labels; live only, no XML)");
  }
  {
    bool bats = ovAnimateBats_;
    if (ImGui::Checkbox("Animate bats", &bats)) setAnimateBats(bats);
    ImGui::SameLine();
    ImGui::TextDisabled("(wing up/down swap like the game; OFF by default; edits always hit the rest pose)");
  }
  {
    float a = scene.parentGhostAlpha();
    bool solid = a >= 0.999f;
    if (ImGui::Checkbox("Solid parent model", &solid))
      scene.setParentGhostAlpha(solid ? 1.f : 0.45f);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100);
    if (ImGui::DragFloat("opacity##parentGhost", &a, 0.01f, 0.1f, 1.f, "%.2f"))
      scene.setParentGhostAlpha(a);
    ImGui::TextDisabled("Parent ghost stays non-selectable either way.");
  }
  if (ImGui::Button("Apply effects to XML")) applyOverrideEffects();

  if (overrideDirty_ || doc.textDirty)
    ImGui::TextColored(ImVec4(1, 0.75f, 0.35f, 1), "Override ops pending — Ctrl+S to save");
  ImGui::TextDisabled("Format: hide/replace/recolor/prop + effect (AODNA loader).");
  ImGui::End();
}

void Editor::openDialog() {
  if (showOpen_) { ImGui::OpenPopup("Open DNA file"); showOpen_ = false; }
  ImGui::SetNextWindowSize(ImVec2(560, 520), ImGuiCond_Appearing);
  if (ImGui::BeginPopupModal("Open DNA file")) {
    ImGui::SetNextItemWidth(-1);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::InputTextWithHint("##of", "filter, e.g. toontown_central", openFilter_, sizeof openFilter_);
    ImGui::BeginChild("fl", ImVec2(0, -40));
    std::string f = openFilter_;
    for (auto &r : files_) {
      if (!f.empty() && r.find(f) == std::string::npos) continue;
      if (ImGui::Selectable(r.c_str(), r == doc.rel, ImGuiSelectableFlags_AllowDoubleClick) && ImGui::IsMouseDoubleClicked(0)) {
        open(r); ImGui::CloseCurrentPopup();
      }
    }
    ImGui::EndChild();
    if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    ImGui::SameLine(); ImGui::TextDisabled("double-click a file to open it");
    ImGui::EndPopup();
  }
}

void Editor::menuBar() {
  if (!ImGui::BeginMenuBar()) return;
  if (ImGui::BeginMenu("File")) {
    if (ImGui::MenuItem("Open...", "Ctrl+O")) showOpen_ = true;
    if (ImGui::MenuItem("Open File (.xml .dna .pdna)...")) {
      std::string p = platform::pickFile("Open DNA file", resRoot, false, platform::kDnaFilters);
      if (!p.empty()) open(p);
    }
    if (ImGui::MenuItem("New Appearance Override...")) newAppearanceOverride();
    if (ImGui::MenuItem("Save", "Ctrl+S", false, !doc.path.empty())) save();
    if (ImGui::MenuItem("Save As (.xml .dna .pdna)...", nullptr, false, !doc.path.empty())) {
      std::string p = platform::pickFile("Save DNA file as (.xml / .dna / .pdna)", doc.path, true, platform::kDnaFilters);
      if (!p.empty()) { if (fs::path(p).extension().empty()) p += fs::path(doc.path).extension().string(); saveAs(p); }
    }
    if (ImGui::MenuItem("Reload from disk", nullptr, false, !doc.path.empty())) open(doc.path);
    ImGui::Separator();
    if (ImGui::MenuItem("Set Resources Folder...")) {
      std::string cur = core::readConfigRes();
      std::string p = platform::pickFolder("Choose your game resources folder", cur);
      if (!p.empty()) { snprintf(resBuf_, sizeof resBuf_, "%s", p.c_str()); resMsg_.clear(); }
      else if (!resBuf_[0]) snprintf(resBuf_, sizeof resBuf_, "%s", cur.c_str());
      showRes_ = true;
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Quit")) quit = true;
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("Edit")) {
    if (ImGui::MenuItem(("Undo " + history.undoLabel()).c_str(), "Ctrl+Z", false, history.canUndo())) history.undo(doc);
    if (ImGui::MenuItem(("Redo " + history.redoLabel()).c_str(), "Ctrl+Y", false, history.canRedo())) history.redo(doc);
    ImGui::Separator();
    if (ImGui::MenuItem("Copy", "Ctrl+C", false, !selection.empty())) copySelected();
    if (ImGui::MenuItem("Paste", "Ctrl+V", false, !clip_.empty())) pasteClipboard(false);
    if (ImGui::MenuItem("Paste at zone level", "Ctrl+Shift+V", false, !clip_.empty())) pasteClipboard(true);
    if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, !selection.empty())) duplicateSelected();
    if (ImGui::MenuItem("Delete", "Del", false, !selection.empty())) deleteSelected();
    if (ImGui::MenuItem("Deselect", "Esc")) selection.clear();
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("Gizmo")) {
    if (ImGui::MenuItem("Drag only", "Q", gizmoOp == render::GizmoOp::None)) gizmoOp = render::GizmoOp::None;
    if (ImGui::MenuItem("Translate", "W", gizmoOp == render::GizmoOp::Translate)) gizmoOp = render::GizmoOp::Translate;
    if (ImGui::MenuItem("Rotate", "E", gizmoOp == render::GizmoOp::Rotate)) { if (gizmoOp != render::GizmoOp::Rotate) prevOp_ = gizmoOp; gizmoOp = render::GizmoOp::Rotate; }
    if (ImGui::MenuItem("Scale", "R", gizmoOp == render::GizmoOp::Scale)) gizmoOp = render::GizmoOp::Scale;
    ImGui::Separator();
    ImGui::MenuItem("Snapping", "X", &snap.on);
    ImGui::SetNextItemWidth(120); ImGui::DragFloat("move step (ft)", &snap.move, 0.05f, 0.05f, 50, "%.2f");
    ImGui::SetNextItemWidth(120); ImGui::DragFloat("angle step", &snap.angle, 0.5f, 1, 90, "%.0f");
    ImGui::SetNextItemWidth(120); ImGui::DragFloat("scale step", &snap.scale, 0.01f, 0.01f, 1, "%.2f");
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("View")) {
    { bool on = core::readConfig("reopen_last") != "0";
      if (ImGui::MenuItem("Reopen last file on startup", nullptr, &on)) core::writeConfig("reopen_last", on ? "1" : "0"); }
    ImGui::Separator();
    auto m = light_.mode;
    if (ImGui::MenuItem("Game lighting (unlit)", "L", m == render::LightMode::Game)) setLight(render::LightMode::Game);
    if (ImGui::MenuItem("Lit (sun + ambient)", nullptr, m == render::LightMode::Lit)) setLight(render::LightMode::Lit);
    if (ImGui::MenuItem("Cog mood (smog, dusk)", nullptr, m == render::LightMode::CogMood)) setLight(render::LightMode::CogMood);
    ImGui::Separator();
    if (ImGui::MenuItem("Invert camera", nullptr, invertCamera_)) {
      invertCamera_ = !invertCamera_;
      core::writeConfig("invert_camera", invertCamera_ ? "1" : "0");
    }
    if (ImGui::MenuItem("Invert camera Y", nullptr, invertCameraY_)) {
      invertCameraY_ = !invertCameraY_;
      core::writeConfig("invert_camera_y", invertCameraY_ ? "1" : "0");
    }
    if (isAppearanceOverride()) {
      bool solid = scene.parentGhostAlpha() >= 0.999f;
      if (ImGui::MenuItem("Solid parent model", nullptr, solid))
        scene.setParentGhostAlpha(solid ? 0.45f : 1.f);
      bool prev = ovPreviewEffects_;
      if (ImGui::MenuItem("Preview effects", nullptr, prev)) setPreviewEffects(!prev);
      bool bats = ovAnimateBats_;
      if (ImGui::MenuItem("Animate bats", nullptr, bats)) setAnimateBats(!bats);
      if (ImGui::BeginMenu("Playground context")) {
        if (ImGui::MenuItem("Show playground (view only, never saved)", nullptr, ctxShow_)) { ctxShow_ = !ctxShow_; rebuildContext(); }
        ImGui::Separator();
        std::string hosts; for (auto &h : ctxHosts_) hosts += (hosts.empty() ? "" : ", ") + h;
        ImGui::TextDisabled("Areas that use %s:", hosts.empty() ? "this zone" : hosts.c_str());
        std::string cur = ctxRel_.empty() ? render::hostDnaRelFor(doc) : ctxRel_;
        for (auto &a : ctxAreas_) if (ImGui::MenuItem(a.c_str(), nullptr, a == cur)) setContextArea(a);
        if (ctxAreas_.empty()) ImGui::TextDisabled("(none found)");
        ImGui::EndMenu();
      }
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Walk through map", "F5", walker.active(), !doc.path.empty())) toggleWalker();
    ImGui::MenuItem("3D view", nullptr, &show3D_);
    ImGui::MenuItem("2D map", nullptr, &showMap_);
    ImGui::MenuItem("Palette", nullptr, &showPalette_);
    ImGui::MenuItem("Visgroups", nullptr, &showVisgroups_);
    ImGui::MenuItem("Inspector", nullptr, &showInspector_);
    ImGui::MenuItem("Objects", nullptr, &showObjects_);
    ImGui::MenuItem("Prop preview", nullptr, &showPreview_);
    ImGui::MenuItem("Log", nullptr, &showLog_);
    ImGui::MenuItem("Timings", nullptr, &showTimings_);
    ImGui::Separator();
    if (ImGui::BeginMenu("Camera speed")) {
      ImGui::TextDisabled("Feet per wheel notch");
      ImGui::SetNextItemWidth(220);
      ImGui::SliderFloat("##wheel-speed", &wheelSpeed, 0.1f, 50.f, "%.1f ft", ImGuiSliderFlags_Logarithmic);
      if (ImGui::IsItemDeactivatedAfterEdit()) core::writeConfig("camera_wheel_speed", dna::fmtNum(wheelSpeed));
      ImGui::TextDisabled("Hold Shift for fine scrolling.");
      if (ImGui::MenuItem("Reset speed")) { wheelSpeed = 1.5f; core::writeConfig("camera_wheel_speed", "1.5"); }
      ImGui::EndMenu();
    }
    if (ImGui::MenuItem("Frame all", "Home")) frameAll();
    if (ImGui::MenuItem("Frame selection", "F")) frameSelection();
    if (ImGui::MenuItem("Reset panel layout")) layoutDone_ = false;
    ImGui::MenuItem("Shortcuts", "F2", &showHelp_);
    ImGui::MenuItem("Input debug line", "F1", &showInputDebug_);
    ImGui::EndMenu();
  }
  ImGui::EndMenuBar();
}

void Editor::shortcuts() {
  ImGuiIO &io = ImGui::GetIO();
  if (io.WantTextInput) return;
  if (ImGui::IsKeyPressed(ImGuiKey_F5, false)) toggleWalker();
  if (walker.active()) return;
  bool c = io.KeyCtrl;
  auto pr = [](ImGuiKey k) { return ImGui::IsKeyPressed(k, false); };
  if (c && pr(ImGuiKey_Z) && !io.KeyShift) history.undo(doc);
  else if ((c && pr(ImGuiKey_Y)) || (c && io.KeyShift && pr(ImGuiKey_Z))) history.redo(doc);
  else if (c && pr(ImGuiKey_S)) save();
  else if (c && pr(ImGuiKey_O)) showOpen_ = true;
  else if (c && pr(ImGuiKey_D)) duplicateSelected();
  else if (c && pr(ImGuiKey_C)) copySelected();
  else if (c && pr(ImGuiKey_V)) pasteClipboard(io.KeyShift);
  else if (!c) {
    if (pr(ImGuiKey_W)) gizmoOp = render::GizmoOp::Translate;
    if (pr(ImGuiKey_E)) {   // toggle: E again returns to the previous mode
      if (gizmoOp == render::GizmoOp::Rotate) gizmoOp = prevOp_ == render::GizmoOp::Rotate ? render::GizmoOp::None : prevOp_;
      else { prevOp_ = gizmoOp; gizmoOp = render::GizmoOp::Rotate; }
    }
    if (pr(ImGuiKey_R)) gizmoOp = render::GizmoOp::Scale;
    if (pr(ImGuiKey_Q)) gizmoOp = render::GizmoOp::None;
    if (pr(ImGuiKey_X)) snap.on = !snap.on;
    if (pr(ImGuiKey_LeftBracket)) yawSelectedBy(io.KeyShift ? -90.f : -yawStep(), true);
    if (pr(ImGuiKey_RightBracket)) yawSelectedBy(io.KeyShift ? 90.f : yawStep(), true);
    if (pr(ImGuiKey_F)) frameSelection();
    if (pr(ImGuiKey_Home)) frameAll();
    if (pr(ImGuiKey_F1)) showInputDebug_ = !showInputDebug_;
    if (pr(ImGuiKey_F2)) showHelp_ = !showHelp_;
    if (pr(ImGuiKey_L)) setLight((render::LightMode)(((int)light_.mode + 1) % 3));
    if (pr(ImGuiKey_Delete) || pr(ImGuiKey_Backspace)) deleteSelected();
    if (pr(ImGuiKey_Escape)) selection.clear();
    // arrow keys nudge (snap step or 0.25 ft)
    float st = snap.on ? snap.move : 0.25f; LVector3 d(0);
    if (pr(ImGuiKey_LeftArrow)) d[0] -= st; if (pr(ImGuiKey_RightArrow)) d[0] += st;
    if (pr(ImGuiKey_UpArrow)) d[1] += st; if (pr(ImGuiKey_DownArrow)) d[1] -= st;
    if (pr(ImGuiKey_PageUp)) d[2] += st; if (pr(ImGuiKey_PageDown)) d[2] -= st;
    if (d.length_squared() > 0) moveSelectedBy(d, true);
  }
}

void Editor::dockLayout(unsigned id) {
  ImGui::DockBuilderRemoveNode(id);
  ImGui::DockBuilderAddNode(id, ImGuiDockNodeFlags_DockSpace);
  ImGui::DockBuilderSetNodeSize(id, ImGui::GetMainViewport()->WorkSize);
  ImGuiID left, right, center = id, bottom;
  left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.19f, nullptr, &center);
  right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.24f, nullptr, &center);
  bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.22f, nullptr, &center);
  ImGuiID leftBottom = ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.35f, nullptr, &left);
  ImGuiID rightBottom = ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.55f, nullptr, &right);
  ImGuiID mapNode = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.42f, nullptr, &center);
  ImGui::DockBuilderDockWindow("3D View", center);
  ImGui::DockBuilderDockWindow("2D Map", mapNode);
  ImGui::DockBuilderDockWindow("Palette", left);
  ImGui::DockBuilderDockWindow("Visgroups", leftBottom);
  ImGui::DockBuilderDockWindow("Inspector", right);
  ImGui::DockBuilderDockWindow("Objects", rightBottom);
  ImGui::DockBuilderDockWindow("Prop Preview", rightBottom);
  ImGui::DockBuilderDockWindow("Log", bottom);
  ImGui::DockBuilderDockWindow("Timings", bottom);
  ImGui::DockBuilderFinish(id);
}

void Editor::statusBar() {
  ImGuiViewport *vp = ImGui::GetMainViewport();
  float h = ImGui::GetFrameHeight();
  ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y + vp->WorkSize.y - h));
  ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, h));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 3));
  ImGui::Begin("##status", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav);
  int live = 0; for (auto &it : doc.items) live += !it.deleted;
  bool dirty = doc.dirty() || overrideDirty_;
  ImGui::TextColored(dirty ? ImVec4(1, 0.75f, 0.35f, 1) : ImVec4(0.55f, 0.85f, 0.6f, 1), dirty ? "\xE2\x97\x8F unsaved" : "\xE2\x97\x8F saved");
  ImGui::SameLine(); ImGui::TextDisabled("%s  |  %d objects  |  %d selected  |  undo %d  |  %.1f ms/frame",
                                         doc.rel.empty() ? "(no file)" : doc.rel.c_str(), live, (int)selection.size(), (int)history.size(), lastFrameMs);
  if (showInputDebug_) {
    auto &in = platform::g_input; ImGuiIO &io = ImGui::GetIO();
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.55f, 0.75f, 1, 1), "|  INPUT mouse %.0f,%.0f %s %s | imgui %.0f,%.0f btn %d%d%d | hovered %s | WantCaptureMouse %d | events %ld last '%s' | display %.0fx%.0f",
                       in.px, in.py, in.inWindow ? "in" : "OUT", in.focused ? "focused" : "UNFOCUSED", io.MousePos.x, io.MousePos.y,
                       io.MouseDown[0], io.MouseDown[1], io.MouseDown[2], hoveredView_.c_str(), io.WantCaptureMouse, in.events, in.last.c_str(),
                       io.DisplaySize.x, io.DisplaySize.y);
  }
  ImGui::End(); ImGui::PopStyleVar();
}

void Editor::frame() {
  hoveredView_ = "none";
  double t0 = core::nowMs();
  ImGuiViewport *vp = ImGui::GetMainViewport();
  float sb = ImGui::GetFrameHeight();
  ImGui::SetNextWindowPos(vp->WorkPos); ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, vp->WorkSize.y - sb));
  ImGui::SetNextWindowViewport(vp->ID);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0)); ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
  ImGui::Begin("##host", nullptr, ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
               ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
               ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoBackground);
  ImGui::PopStyleVar(2);
  ImGuiID ds = ImGui::GetID("MainDock");
  if (!layoutDone_) { dockLayout(ds); layoutDone_ = true; }
  ImGui::DockSpace(ds, ImVec2(0, 0), ImGuiDockNodeFlags_None);
  menuBar();
  ImGui::End();

  viewport3D();
  viewport2D();

  if (showPalette_) {
    ImGui::Begin("Palette", &showPalette_);
    if (showPalette_) {
      int clicked = ui::palettePanel(catalog, pal_, loadedCodes_);
      if (clicked >= 0) { showPreview_ = true; openPreview(clicked); ImGui::SetWindowFocus("Prop Preview"); }
    }
    ImGui::End();
  }
  previewWindow();

  if (showInspector_) {
  ImGui::Begin("Inspector", &showInspector_);
  if (showInspector_) {
  ImGui::BeginChild("insp-scroll");
  dna::Item *it = selection.size() == 1 ? &doc.items[selection[0]] : nullptr;
  if (selection.size() > 1) ImGui::TextDisabled("%d objects selected.\nThe gizmo moves the first. A drag moves all of them.", (int)selection.size());
  else {
    dna::Xform x = it ? it->xf : dna::Xform{};
    ui::SignEdit sign;
    int r = ui::inspectorPanel(it, it ? selection[0] : -1, x, &doc, &sign);
    if (sign.commit) {
      auto apply = [&](dna::Document &target) {
        if (sign.baseline) return target.setSignLine(sign.baseline, sign.text);
        if (sign.key == "@text") return target.setNodeText(sign.node, sign.value);
        return target.setNodeAttribute(sign.node, sign.key, sign.value);
      };
      bool changed = false;
      if (it && it->origId < 0) {
        dna::Document temporary; temporary.fmt = dna::Format::Xml; temporary.text = it->templateXml;
        auto command = std::make_unique<commands::TemplateCmd>();
        command->id = selection[0]; command->before = it->templateXml;
        if (apply(temporary) && doc.setItemTemplate(command->id, temporary.text)) {
          command->after = temporary.text; history.pushApplied(std::move(command)); changed = true;
        }
      } else {
        auto command = std::make_unique<commands::SourceCmd>();
        command->before = doc.text; command->beforeDirty = doc.textDirty;
        command->what = sign.baseline ? "Edit sign text" : "Edit property";
        if (apply(doc)) { command->after = doc.text; history.pushApplied(std::move(command)); changed = true; }
      }
      if (changed) {
        rebuildScene();
        it = selection.size() == 1 ? &doc.items[selection[0]] : nullptr;
      }
    }
    if (r && it) {
      if (!editing_) beginEdit();
      it->xf = x; scene.syncItem(doc, selection[0]);
      if (r == 2) endEdit("Edit");
    }
  }
  ImGui::Dummy(ImVec2(0, 8));
  ImGui::SeparatorText("Tools");
  ImGui::TextDisabled("Gizmo");
  int op = (int)gizmoOp;
  ImGui::RadioButton("Off", &op, 0); ImGui::SameLine();
  ImGui::RadioButton("Move", &op, 1); ImGui::SameLine();
  ImGui::RadioButton("Rotate", &op, 2); ImGui::SameLine();
  ImGui::RadioButton("Scale", &op, 3); gizmoOp = (render::GizmoOp)op;
  ImGui::Dummy(ImVec2(0, 4));
  ImGui::Checkbox("Snap", &snap.on);
  ImGui::TextDisabled("Move step"); ImGui::SameLine(108); ImGui::SetNextItemWidth(-1); ImGui::DragFloat("##snapft", &snap.move, 0.05f, 0.05f, 50, "%.2f ft");
  ImGui::TextDisabled("Angle step");
  for (float a : {15.f, 45.f, 90.f}) { ImGui::SameLine(); char b[16]; snprintf(b, 16, "%.0f deg", a); if (ImGui::RadioButton(b, snap.angle == a)) snap.angle = a; }
  if (!selection.empty()) {
    ImGui::Dummy(ImVec2(0, 6));
    ImGui::TextDisabled("Yaw");
    if (ImGui::Button("-90")) yawSelectedBy(-90, true); ImGui::SameLine();
    if (ImGui::Button("-step")) yawSelectedBy(-yawStep(), true); ImGui::SameLine();
    if (ImGui::Button("+step")) yawSelectedBy(yawStep(), true); ImGui::SameLine();
    if (ImGui::Button("+90")) yawSelectedBy(90, true);
    if (ImGui::Button("Reset rotation")) { beginEdit(); for (int i : selection) { auto &xf = doc.items[i].xf; xf.h = xf.p = xf.r = 0; scene.syncItem(doc, i); } endEdit("Rotate"); }
    if (ImGui::Button("Duplicate")) duplicateSelected();
    ImGui::SameLine(); if (ImGui::Button("Delete")) deleteSelected();
    ImGui::SameLine(); if (ImGui::Button("Frame")) frameSelection();
  }
  ImGui::EndChild();
  }
  ImGui::End();
  }

  if (showObjects_) {
    ImGui::Begin("Objects", &showObjects_);
    if (showObjects_) {
      bool focus = false;
      ui::outlinerPanel(doc, selection, out_, &focus);
      if (focus) frameSelection();
    }
    ImGui::End();
  }

  visgroupsPanel();
  if (showLog_) { ImGui::Begin("Log", &showLog_); if (showLog_) ui::logPanel(&logAuto_); ImGui::End(); }
  statsPanel();
  overridePanel();
  updateEffectsPreview();
  updateBatsPreview();
  if (showHelp_) {
    ImGui::SetNextWindowSize(ImVec2(610, std::min(760.f, vp->WorkSize.y - 60)), ImGuiCond_FirstUseEver);
    ImGui::Begin("Shortcuts", &showHelp_); ui::helpPanel(); ImGui::End();
  }
  openDialog();
  resDialog();
  passwordDialog();
  statusBar();
  shortcuts();
  lastFrameMs = app_.frameMs;
  core::setTiming("ui build (per frame)", core::nowMs() - t0);
  static bool stateLog = getenv("DNAED_INPUT_LOG") != nullptr;
  if (stateLog) {   // real-input diagnostics: print editor state whenever it changes
    std::string sel; for (int i : selection) sel += std::to_string(i) + " ";
    std::string st = fmt("hpr %s | 3D yaw %.0f pitch %.0f dist %.0f tgt %.0f,%.0f | 2D span %.0f tgt %.0f,%.0f | sel [%s] | light %d | gizmo %d | popup %d | undo %d",
                         selection.empty() ? "-" : fmt("%.1f,%.1f,%.1f", doc.items[selection[0]].xf.h, doc.items[selection[0]].xf.p, doc.items[selection[0]].xf.r).c_str(), v3->yaw, v3->pitch, v3->dist, v3->target[0], v3->target[1], v2->span, v2->target[0], v2->target[1], sel.c_str(),
                         (int)light_.mode, (int)gizmoOp, (int)ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel), (int)history.size());
    static std::string last; if (st != last) { fprintf(stderr, "[state] %s\n", st.c_str()); last = st; }
  }
}

void Editor::resDialog() {
  if (showRes_) { ImGui::OpenPopup("Resources folder"); showRes_ = false; }
  ImGui::SetNextWindowSize(ImVec2(620, 0), ImGuiCond_Appearing);
  if (!ImGui::BeginPopupModal("Resources folder", nullptr, 0)) return;
  ImGui::TextWrapped("The game's resources folder (contains phase_4, phase_8, ...). Saved to %s; used on next start "
                     "unless --res or DNAED_RES is given.", core::configPath().c_str());
  ImGui::SetNextItemWidth(-90); ImGui::InputText("##res", resBuf_, sizeof resBuf_); ImGui::SameLine();
  if (ImGui::Button("Browse...")) { std::string p = platform::pickFolder("Choose the resources folder", resBuf_); if (!p.empty()) snprintf(resBuf_, sizeof resBuf_, "%s", p.c_str()); }
  // simple built-in browser (fallback when no native picker): list sub-folders of the typed path
  std::error_code ec; std::filesystem::path cur(resBuf_);
  if (std::filesystem::is_directory(cur, ec)) {
    ImGui::BeginChild("dirs", ImVec2(0, 160), ImGuiChildFlags_Borders);
    if (ImGui::Selectable("..")) snprintf(resBuf_, sizeof resBuf_, "%s", cur.parent_path().string().c_str());
    int n = 0;
    for (auto &e : std::filesystem::directory_iterator(cur, std::filesystem::directory_options::skip_permission_denied, ec)) {
      if (!e.is_directory(ec) || n++ > 300) continue;
      std::string name = e.path().filename().string();
      if (ImGui::Selectable((name + "/").c_str())) { snprintf(resBuf_, sizeof resBuf_, "%s", e.path().string().c_str()); break; }
    }
    ImGui::EndChild();
  }
  bool ok = core::isResourcesDir(resBuf_);
  ImGui::TextColored(ok ? ImVec4(0.4f, 0.9f, 0.5f, 1) : ImVec4(1, 0.6f, 0.3f, 1), ok ? "looks like a resources folder" : "no phase_4/phase_8 inside");
  if (!resMsg_.empty()) ImGui::TextUnformatted(resMsg_.c_str());
  ImGui::BeginDisabled(!ok);
  if (ImGui::Button("Save")) resMsg_ = core::writeConfigRes(resBuf_) ? "Saved. Restart the editor to load it." : "Could not write the config file.";
  ImGui::EndDisabled(); ImGui::SameLine();
  if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
  ImGui::EndPopup();
}

// ---------------------------------------------------------------- copy / paste anywhere
void Editor::copySelected() {
  clip_.clear();
  for (int i : selection) {
    if (i < 0 || i >= (int)doc.items.size()) continue;
    const auto &it = doc.items[i];
    if (it.deleted) continue;
    if (it.nested) { core::warn("copy: '" + it.name + "' is nested inside another object; copy its parent instead"); continue; }
    ClipItem c; c.xml = doc.itemXml(it); c.kind = it.kind; c.name = it.name; c.code = it.code; c.visgroup = it.visgroup;
    c.xf = it.xf; c.origXf = it.origXf;
    NodePath n = i < (int)scene.itemNodes.size() ? scene.itemNodes[i] : NodePath();
    c.world = n.is_empty() ? LMatrix4::ident_mat() : n.get_mat(scene.world);
    clip_.push_back(std::move(c));
  }
  core::info(fmt("copied %d object(s)", (int)clip_.size()));
}

LPoint3 Editor::clipboardCenter() const {
  LPoint3 c(0, 0, 0); if (clip_.empty()) return c;
  for (auto &k : clip_) c += k.world.get_row3(3);
  return c / (PN_stdfloat)clip_.size();
}

static std::string attrOfFirstTag(const std::string &xml, const std::string &a) {
  size_t end = xml.find('>'); std::string head = xml.substr(0, end);
  std::smatch m; std::regex re("\\s" + a + "=\"([^\"]*)\"");
  return std::regex_search(head, m, re) ? m[1].str() : std::string();
}
static std::string dropAttrsOfFirstTag(std::string xml, std::initializer_list<const char *> names) {
  size_t end = xml.find('>'); std::string head = xml.substr(0, end), tail = end == std::string::npos ? "" : xml.substr(end);
  for (auto n : names) head = std::regex_replace(head, std::regex(std::string("\\s") + n + "=\"[^\"]*\""), "");
  return head + tail;
}

int Editor::pasteClipboard(bool zoneLevel, const LPoint3 *at) {
  if (clip_.empty()) { core::warn("paste: clipboard is empty (Ctrl+C a prop first)"); return 0; }
  // Target: explicit point, else under the mouse in the 3D view, else the view centre.
  LPoint3 target;
  if (at) target = *at;
  else {
    ImVec2 mp = ImGui::GetIO().MousePos; render::Rect r = v3->rect();
    bool in = mp.x >= r.x && mp.y >= r.y && mp.x < r.x + r.w && mp.y < r.y + r.h;
    render::PickHit h; if (in) h = render::pick(*v3, mp.x, mp.y, scene.geom);
    LPoint3 o, p; LVector3 d;
    if (in && h.hit) target = h.point;
    else if (in && v3->ray(mp.x, mp.y, o, d) && render::rayPlaneZ(o, d, 0, p)) target = p;
    else target = placementPoint();
    if (snap.on) { float s = std::max(0.05f, snap.move); target[0] = std::round(target[0] / s) * s; target[1] = std::round(target[1] / s) * s; }
  }
  LVector3 off = target - clipboardCenter();
  std::vector<int> added;
  for (auto &c : clip_) {
    dna::Item it; it.kind = c.kind; it.name = c.name; it.code = c.code; it.origId = -1;
    it.visgroup = std::find(doc.visgroups.begin(), doc.visgroups.end(), c.visgroup) != doc.visgroups.end() ? c.visgroup
                : (!selection.empty() ? doc.items[selection[0]].visgroup : (doc.visgroups.empty() ? "" : doc.visgroups[0]));
    it.templateXml = c.xml; it.origXf = c.origXf; it.xf = c.xf;
    std::string pc = attrOfFirstTag(c.xml, "parent_code");
    NodePath frame = pc.empty() ? NodePath() : scene.attachParent(pc);
    if (!pc.empty() && (zoneLevel || frame.is_empty())) {
      // Zone-level: detach from the parent model; the game parents these under the zone geom.
      it.templateXml = dropAttrsOfFirstTag(c.xml, {"parent_code", "parent_node"});
      frame = NodePath();
    }
    if (frame.is_empty()) frame = scene.visgroupNodes.count(it.visgroup) ? scene.visgroupNodes[it.visgroup]
                                : (scene.visgroupNodes.empty() ? scene.geom : scene.visgroupNodes.begin()->second);
    LMatrix4 m = c.world; m.set_row(3, m.get_row3(3) + off);
    NodePath tmp = scene.world.attach_new_node("paste_tmp"); tmp.set_mat(m);
    LPoint3 p = tmp.get_pos(frame); LVecBase3 hpr = tmp.get_hpr(frame), s = tmp.get_scale(frame);
    tmp.remove_node();
    it.xf.x = p[0]; it.xf.y = p[1]; it.xf.z = p[2]; it.xf.h = hpr[0]; it.xf.p = hpr[1]; it.xf.r = hpr[2];
    it.xf.sx = s[0]; it.xf.sy = s[1]; it.xf.sz = s[2];
    doc.items.push_back(it); int id = (int)doc.items.size() - 1;
    scene.buildAdded(doc, id, storage_); added.push_back(id);
  }
  if (added.empty()) return 0;
  auto cmd = std::make_unique<commands::DeleteCmd>(); cmd->ids = added; cmd->add = true;
  history.pushApplied(std::move(cmd));
  selection = added;
  core::info(fmt("pasted %d object(s)%s at (%.1f, %.1f, %.1f)", (int)added.size(), zoneLevel ? " at zone level" : "",
                 target[0], target[1], target[2]));
  return (int)added.size();
}

// ---------------------------------------------------------------- playground context (Appearance Override)
void Editor::scanContextAreas() {
  ctxAreas_.clear(); ctxHosts_.clear();
  if (!isAppearanceOverride() || !doc.root) return;
  std::set<std::string> hosts;
  dna::walk(doc.root.get(), [&](const dna::XmlNode *n) { auto pc = n->attr("parent_code"); if (!pc.empty()) hosts.insert(pc); });
  ctxHosts_.assign(hosts.begin(), hosts.end());
  std::string def = render::hostDnaRelFor(doc);
  for (auto &rel : dna::sceneFiles(resRoot)) {
    if (rel.find("/overrides/") != std::string::npos) continue;
    bool use = rel == def;
    if (!use && !hosts.empty()) {
      std::ifstream f(fs::path(resRoot) / rel); std::string t((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
      for (auto &h : hosts) if (t.find("code=\"" + h + "\"") != std::string::npos) { use = true; break; }
    }
    if (use) ctxAreas_.push_back(rel);
  }
  std::sort(ctxAreas_.begin(), ctxAreas_.end());
}

void Editor::detachContext() { if (ctxBuilt_ && !ctxScene_.geom.is_empty()) ctxScene_.geom.detach_node(); }
void Editor::attachContext() {
  if (!ctxBuilt_ || ctxScene_.geom.is_empty() || !ctxShow_) return;
  ctxScene_.geom.reparent_to(scene.geom);
}

void Editor::rebuildContext() {
  detachContext();
  if (ctxBuilt_) { ctxScene_.clear(); ctxBuilt_ = false; }
  if (!ctxShow_ || !isAppearanceOverride() || !doc.root) return;
  std::string rel = ctxRel_.empty() ? render::hostDnaRelFor(doc) : ctxRel_;
  if (rel.empty()) return;
  ScopedTimer t("playground context");
  dna::Document d; std::string err;
  if (!d.load((fs::path(resRoot) / rel).string(), resRoot, &err)) { core::warn("context: cannot load " + rel + ": " + err); return; }
  ctxDoc_ = std::move(d);
  auto st = dna::storagesForFile(resRoot, ctxDoc_.rel, ctxDoc_.text, nullptr);
  ctxStorage_.clear(); ctxStorage_.addFiles(st);
  ctxScene_.build(ctxDoc_, ctxStorage_);
  // The parent model already stands in the scene (solid/ghost attach parent): hide its context copy.
  int hidden = 0;
  for (size_t i = 0; i < ctxDoc_.items.size() && i < ctxScene_.itemNodes.size(); i++)
    if (std::find(ctxHosts_.begin(), ctxHosts_.end(), ctxDoc_.items[i].code) != ctxHosts_.end() && !ctxScene_.itemNodes[i].is_empty()) {
      ctxScene_.itemNodes[i].stash(); hidden++; }
  // Never selectable: drop every editable-id tag so picks fall through to "nothing".
  auto tagged = ctxScene_.geom.find_all_matches("**/=eid");
  for (int k = 0; k < tagged.get_num_paths(); k++) tagged.get_path(k).clear_tag("eid");
  ctxScene_.geom.set_name("playground_context (not saved)");
  ctxBuilt_ = true;
  attachContext();
  core::info(fmt("context: %s (%d objects, parent copy hidden: %d) - view only, never saved", rel.c_str(), (int)ctxDoc_.items.size(), hidden));
}

void Editor::setContextArea(const std::string &rel) {
  ctxRel_ = rel; render::setHostDnaOverride(rel);
  detachContext(); rebuildScene(); rebuildContext();
}
