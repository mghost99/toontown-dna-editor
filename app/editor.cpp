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
#include <cmath>
#include <filesystem>

namespace fs = std::filesystem;
using core::ScopedTimer;

static std::string fmt(const char *f, ...) {
  char b[1024]; va_list a; va_start(a, f); vsnprintf(b, sizeof b, f, a); va_end(a); return b;
}

Editor::Editor(platform::App &app, std::string res) : resRoot(std::move(res)), app_(app) {
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
  history.onChanged = [this](const std::vector<int> &ids) { syncItems(ids); };
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
  doc = std::move(nd);
  std::vector<std::string> undef;
  auto st = dna::storagesForFile(resRoot, doc.rel, doc.text, &undef);
  storage_.clear(); storage_.addFiles(st);
  std::string sl; for (auto &s : st) sl += (sl.empty() ? "" : ", ") + fs::path(s).filename().string();
  core::info("storage for " + doc.rel + ": " + sl);
  for (auto &u : undef) core::warn("code used by this file but defined in no storage file: " + u);
  history.clear(); selection.clear(); boxes_.clear(); editing_ = false; dragging_ = dragArmed_ = false;
  { ScopedTimer t("generate scene (models+textures)"); scene.build(doc, storage_); }
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
  if (fromGizmo && snap.on) {   // float noise from matrix decomposition: land exactly on the snap steps
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
  if (!previewModel_.is_empty()) previewModel_.remove_node();
  NodePath src = assets::loadStoreNode(e.model, e.node);
  previewIdx_ = ci; pvYaw_ = 35; pvPitch_ = 20; pvZoom_ = 1;
  if (src.is_empty()) { core::warn("preview: cannot load " + e.code + " (" + e.model + ")"); previewModel_ = previewRoot_.attach_new_node("empty"); previewSize_ = LVecBase3(0); previewTris_ = 0; return false; }
  previewModel_ = src.copy_to(previewRoot_);
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
  if (!loadedCodes_.count(e.code)) {
    storage_.addCode(e.code, e.model, e.node, e.storageRel); loadedCodes_.insert(e.code);
    core::warn("placed " + e.code + " from " + e.storageRel + ", which this DNA's storage files do not load: the game will not show it unless that storage is added");
  }
  dna::Item it; it.kind = "prop"; it.code = e.code; it.name = e.code;
  it.visgroup = !selection.empty() ? doc.items[selection[0]].visgroup : (doc.visgroups.empty() ? "" : doc.visgroups[0]);
  it.templateXml = "<prop name=\"" + e.code + "\" code=\"" + e.code + "\">\n\t<pos x=\"0.00\" y=\"0.00\" z=\"0.00\" />\n\t<nhpr h=\"0.00\" p=\"0.00\" r=\"0.00\" />\n</prop>";
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
float Editor::snapv(float v) const { return snap.on && snap.move > 0 ? std::round(v / snap.move) * snap.move : v; }

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
      for (size_t k = 0; k < editIds_.size(); k++) { auto &x = doc.items[editIds_[k]].xf; x.h += (io.MouseWheel > 0 ? st : -st); if (snap.on) x.h = std::round(x.h / st) * st; scene.syncItem(doc, editIds_[k]); }
      wheelUsed_ = true;
    }
    if (dragging_ && rotDrag_) {   // Shift+drag: horizontal motion = yaw (0.5 deg/px), snapped
      double t0 = core::nowMs();
      float raw = dx * 0.5f * (topDown ? -1.f : -1.f);
      float st = snap.on && snap.angle > 0 ? snap.angle : 0.f;
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
    using_ = render::gizmo(*v3, scene.itemNodes[p0], gizmoOp, snap, &gizmoHover, &gChanged, (bx.mn + bx.mx) * 0.5f);
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
    // One light notch is a small step. A harder flick stacks wheel units, and the curve grows slower than that stack.
    float mag = std::pow(std::fabs(io.MouseWheel), 0.65f);
    float step = v3->dist * 0.02f * mag;
    v3->target += viewFwd() * (io.MouseWheel > 0 ? step : -step);
  }
  bool pan = own && (ImGui::IsMouseDown(2) || (ImGui::IsMouseDown(1) && io.KeyShift));
  if (own && ImGui::IsMouseDown(1) && !io.KeyShift) orbiting_ = true;
  if (!own || (!ImGui::IsMouseDown(1) && !ImGui::IsMouseDown(2))) orbiting_ = false;
  if (own && (orbiting_ || pan) && (io.MouseDelta.x || io.MouseDelta.y)) {
    if (pan) {
      LVector3 right = scene.world.get_relative_vector(v3->cam, LVector3(1, 0, 0)), up = scene.world.get_relative_vector(v3->cam, LVector3(0, 0, 1));
      float k = std::max(v3->dist, 12.f) * 0.0016f;
      v3->target -= right * io.MouseDelta.x * k; v3->target += up * io.MouseDelta.y * k;
    } else if (orbiting_) {
      LPoint3 camPos = v3->target - viewFwd() * v3->dist;
      v3->yaw -= io.MouseDelta.x * 0.3f; v3->pitch -= io.MouseDelta.y * 0.3f;
      v3->target = camPos + viewFwd() * v3->dist;
    }
  }
  if (hovered && ImGui::IsKeyDown(ImGuiKey_MouseRight) == false) {}
  v3->apply();
  drawSelection(*v3, false);
  // overlay
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const char *ops[] = {"Q drag", "W move", "E rotate", "R scale"};
  std::string hud = fmt("%s   |   snap %s (X)   |   %s   |   %.1f ms/frame", ops[(int)gizmoOp], snap.on ? "on" : "off",
                        light_.mode == render::LightMode::Game ? "Game light" : light_.mode == render::LightMode::Lit ? "Lit" : "Cog mood", lastFrameMs);
  dl->AddRectFilled(ImVec2(p.x + 8, p.y + 8), ImVec2(p.x + 16 + ImGui::CalcTextSize(hud.c_str()).x, p.y + 32), IM_COL32(15, 16, 20, 190), 5);
  dl->AddText(ImVec2(p.x + 12, p.y + 12), IM_COL32(220, 225, 235, 255), hud.c_str());
  ImGui::End();
}

void Editor::viewport2D() {
  if (!showMap_) { v2->setActive(false); return; }
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
  if ((own || (hovered && pointerView_ == 0)) && io.MouseWheel != 0 && !wheelUsed_) {   // zoom around the cursor
    LPoint3 o, before, after; LVector3 d;
    bool a = v2->ray(io.MousePos.x, io.MousePos.y, o, d) && render::rayPlaneZ(o, d, 0, before);
    float mag = std::copysign(std::pow(std::fabs(io.MouseWheel), 0.65f), io.MouseWheel);
    v2->span *= std::pow(0.97f, mag);
    if (v2->span < 0.5f) v2->span = 0.5f;
    v2->apply();
    if (a && v2->ray(io.MousePos.x, io.MousePos.y, o, d) && render::rayPlaneZ(o, d, 0, after)) { v2->target += before - after; v2->target[2] = 0; }
  }
  if (own && (ImGui::IsMouseDown(1) || ImGui::IsMouseDown(2)) && (io.MouseDelta.x || io.MouseDelta.y)) {
    float k = v2->span / std::max(1.f, std::min(s.x, s.y));
    v2->target[0] -= io.MouseDelta.x * k; v2->target[1] += io.MouseDelta.y * k;
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
  bool c = io.KeyCtrl;
  auto pr = [](ImGuiKey k) { return ImGui::IsKeyPressed(k, false); };
  if (c && pr(ImGuiKey_Z) && !io.KeyShift) history.undo(doc);
  else if ((c && pr(ImGuiKey_Y)) || (c && io.KeyShift && pr(ImGuiKey_Z))) history.redo(doc);
  else if (c && pr(ImGuiKey_S)) save();
  else if (c && pr(ImGuiKey_O)) showOpen_ = true;
  else if (c && pr(ImGuiKey_D)) duplicateSelected();
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
  ImGui::TextColored(doc.dirty() ? ImVec4(1, 0.75f, 0.35f, 1) : ImVec4(0.55f, 0.85f, 0.6f, 1), doc.dirty() ? "\xE2\x97\x8F unsaved" : "\xE2\x97\x8F saved");
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
    if (sign.commit && sign.baseline) {
      int keep = selection[0];
      if (doc.setSignLine(sign.baseline, sign.text)) {
        scene.build(doc, storage_);
        if (keep >= 0 && keep < (int)doc.items.size()) selection = {keep};
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
  if (showHelp_) { ImGui::Begin("Shortcuts", &showHelp_, ImGuiWindowFlags_AlwaysAutoResize); ui::helpPanel(); ImGui::End(); }
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
