#include "scene.h"
#include "core/log.h"
#include "dna/sign_fonts.h"
#include "geomNode.h"
#include "geomTriangles.h"
#include "geomVertexWriter.h"
#include "modelNode.h"
#include "textureStage.h"
#include "depthOffsetAttrib.h"
#include "collisionNode.h"
#include "collisionPlane.h"
#include "compress_string.h"
#include "textNode.h"
#include "fontPool.h"
#include "bamFile.h"
#include "cardMaker.h"
#include "transparencyAttrib.h"
#include "billboardEffect.h"
#include "textureAttrib.h"
#include <filesystem>
#include <sstream>
#include <fstream>
#include <cstring>
#include <cctype>
#include <cmath>
#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <vector>

namespace render {

static std::string unb64(const std::string &in) {
  std::string o; uint32_t v = 0; int b = -8;
  for (unsigned char c : in) {
    int d = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52 : c == '+' ? 62 : c == '/' ? 63 : -1;
    if (d < 0) continue; v = (v << 6) + d; b += 6; if (b >= 0) { o += char((v >> b) & 255); b -= 8; }
  }
  return o;
}
static double fl(const std::string &s, double d) { if (s.empty()) return d; char *e; double v = strtod(s.c_str(), &e); return e == s.c_str() || !std::isfinite(v) ? d : v; }

Scene::Scene() { world = NodePath("world"); geom = world.attach_new_node("dna"); markers = world.attach_new_node("markers"); }

void Scene::clear() {
  assets::stopAnimations(animations);
  geom.remove_node(); markers.remove_node();
  geom = world.attach_new_node("dna"); markers = world.attach_new_node("markers");
  itemNodes.clear(); visgroupNodes.clear(); unknownTags_.clear(); attachParents_.clear(); walkFloor_ = NodePath(); stats_nodes = 0;
}


namespace fs = std::filesystem;

// Host playground DNA for Appearance Override files (parent_code world xform).
static std::string hostDnaRel(const dna::Document &doc) {
  std::string zone = doc.root ? doc.root->attr("zone") : "";
  auto base = fs::path(doc.rel).filename().string();
  for (auto &c : base) c = (char)tolower((unsigned char)c);
  if (zone == "2000" || zone == "ToontownCentral" || base.rfind("ttc", 0) == 0)
    return "phase_4/dna/toontown_central_sz.xml";
  if (zone == "1000" || base.rfind("dd", 0) == 0) return "phase_5/dna/donalds_dock_sz.xml";
  if (zone == "3000" || base.rfind("dg", 0) == 0) return "phase_5/dna/daisys_garden_sz.xml";
  if (zone == "4000" || base.rfind("mm", 0) == 0) return "phase_6/dna/minnies_melodyland_sz.xml";
  if (zone == "5000" || base.rfind("br", 0) == 0 || base.rfind("tb", 0) == 0)
    return "phase_6/dna/the_burrrgh_sz.xml";
  if (zone == "6000" || base.rfind("dl", 0) == 0) return "phase_8/dna/donalds_dreamland_sz.xml";
  return "";
}

static bool lookupHostPropXform(const dna::Document &doc, const std::string &code,
                                LPoint3 &pos, LVecBase3 &hpr) {
  pos = LPoint3(0, 0, 0); hpr = LVecBase3(0, 0, 0);
  std::string rel = hostDnaRel(doc);
  if (rel.empty()) return false;
  std::string path = assets::ModelCache::get().resRoot() + "/" + rel;
  std::string err, text;
  // Lightweight: read file and scan for code= then nearby pos/nhpr (avoid full Document load).
  std::ifstream in(path);
  if (!in) return false;
  std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  std::string needle = "code=\"" + code + "\"";
  size_t p = all.find(needle);
  if (p == std::string::npos) return false;
  // Search forward within this prop element for <pos and <nhpr/<hpr
  size_t end = all.find("</prop>", p);
  if (end == std::string::npos) end = std::min(all.size(), p + 800);
  auto grab = [&](const char *tag, const char *ax, float &out) {
    size_t a = all.find(std::string("<") + tag, p);
    if (a == std::string::npos || a > end) return;
    size_t b = all.find(std::string(ax) + "=\"", a);
    if (b == std::string::npos || b > end) return;
    b += strlen(ax) + 2;
    out = (float)atof(all.c_str() + b);
  };
  float x = 0, y = 0, z = 0, h = 0, pp = 0, r = 0;
  grab("pos", "x", x); grab("pos", "y", y); grab("pos", "z", z);
  if (all.find("<nhpr", p) < end) { grab("nhpr", "h", h); grab("nhpr", "p", pp); grab("nhpr", "r", r); }
  else { grab("hpr", "h", h); grab("hpr", "p", pp); grab("hpr", "r", r); }
  pos = LPoint3(x, y, z); hpr = LVecBase3(h, pp, r);
  return true;
}

static NodePath makeTexturedCard(const std::string &texPath, const std::string &name, bool billboard) {
  PT(Texture) tex = assets::ModelCache::get().texture(texPath);
  CardMaker cm(name.empty() ? "aodna_card" : name);
  std::string low = name + texPath;
  for (auto &c : low) c = (char)tolower((unsigned char)c);
  if (low.find("bat") != std::string::npos) cm.set_frame(-0.5f, 0.5f, -0.35f, 0.35f);
  else cm.set_frame(-1.0f, 1.0f, -1.0f, 1.0f);
  NodePath card(cm.generate());
  if (tex) card.set_texture(tex, 1);
  card.set_transparency(TransparencyAttrib::M_alpha);
  card.set_two_sided(true);
  card.set_light_off(1);
  card.set_color_scale_off(1);
  card.set_depth_write(false);
  card.set_bin("transparent", 20);
  if (billboard) card.set_effect(BillboardEffect::make_point_eye());
  return card;
}

void Scene::prepareAttachParents(const dna::Document &doc, assets::Storage &st) {
  // Collect parent_code values from editable props so holders exist before gen.
  std::set<std::string> codes;
  dna::walk(doc.root.get(), [&](const dna::XmlNode *n) {
    if (n->tag != "prop" && n->tag != "anim_prop" && n->tag != "interactive_prop") return;
    std::string pc = n->attr("parent_code");
    if (!pc.empty()) codes.insert(pc);
  });
  for (auto &c : codes) ensureAttachParent(c, st, &doc);
  ensureWalkFloor(doc);
}

NodePath Scene::ensureAttachParent(const std::string &code, assets::Storage &st, const dna::Document *doc) {
  auto it = attachParents_.find(code);
  if (it != attachParents_.end() && !it->second.is_empty()) return it->second;
  NodePath holder = geom.attach_new_node("aodna_parent_" + code);
  LPoint3 pos(0, 0, 0); LVecBase3 hpr(0, 0, 0);
  bool found = doc && lookupHostPropXform(*doc, code, pos, hpr);
  if (found) {
    holder.set_pos(pos);
    holder.set_hpr(hpr);
  } else {
    core::warn("ATTACH-TO-MODEL: no host xform for parent_code=" + code + " (props at local origin)");
  }
  // Parent model for placement context (not an editable item / no eid — non-selectable).
  NodePath src = st.findNode(code);
  if (!src.is_empty()) {
    NodePath ghost = src.copy_to(holder);
    ghost.set_name("aodna_parent_ghost");
    ghost.set_light_off(1);
    { auto cc = ghost.find_all_matches("**/+CollisionNode"); cc.hide(); }
    applyParentGhostAlpha(ghost);
  } else {
    core::warn("ATTACH-TO-MODEL: parent model not in storage: " + code);
  }
  attachParents_[code] = holder;
  return holder;
}

void Scene::applyParentGhostAlpha(NodePath ghost) const {
  if (ghost.is_empty()) return;
  float a = parentGhostAlpha_;
  if (a >= 0.999f) {
    ghost.clear_transparency();
    ghost.clear_color_scale();
  } else {
    ghost.set_transparency(TransparencyAttrib::M_alpha);
    ghost.set_color_scale(1, 1, 1, std::max(0.1f, a));
  }
}

void Scene::setParentGhostAlpha(float a) {
  parentGhostAlpha_ = std::clamp(a, 0.1f, 1.f);
  for (auto &kv : attachParents_) {
    if (kv.second.is_empty()) continue;
    NodePath ghost = kv.second.find("**/aodna_parent_ghost");
    if (ghost.is_empty()) ghost = kv.second.find("aodna_parent_ghost");
    applyParentGhostAlpha(ghost);
  }
}

void Scene::ensureWalkFloor(const dna::Document &doc) {
  // Appearance Override files only contain overlay props — no street/ground.
  // Add a large collidable plane so Walk mode has a floor. Never an editable
  // item (no eid tag) and never written to XML.
  if (!walkFloor_.is_empty()) { walkFloor_.remove_node(); walkFloor_ = NodePath(); }
  bool need = false;
  if (doc.root) {
    std::string kind = doc.root->attr("kind");
    if (kind == "appearance_override" || kind == "AppearanceOverride") need = true;
    dna::walk(doc.root.get(), [&](const dna::XmlNode *n) {
      if (n && !n->attr("parent_code").empty()) need = true;
    });
  }
  if (!need) return;
  // Ground height: use host gazebo/base z when known; TTC gazebo sits at z≈-2.
  float z = -2.f;
  LPoint3 pos; LVecBase3 hpr;
  if (lookupHostPropXform(doc, "prop_gazebo", pos, hpr))
    z = pos[2];
  walkFloor_ = geom.attach_new_node("aodna_walk_floor");
  PT(CollisionNode) cn = new CollisionNode("aodna_walk_floor_col");
  cn->set_into_collide_mask(BitMask32::all_on());
  cn->set_from_collide_mask(BitMask32::all_off());
  // Huge plane at playground ground height (gazebo base).
  cn->add_solid(new CollisionPlane(LPlane(LVector3(0, 0, 1), LPoint3(0, 0, z))));
  NodePath col = walkFloor_.attach_new_node(cn);
  col.hide();  // collision only — no selectable geometry
  core::info("AODNA walk floor at z=" + std::to_string(z) + " (context only, not saved)");
}


NodePath makeMarker(const LColor &c, float s) {
  PT(GeomVertexData) vd = new GeomVertexData("m", GeomVertexFormat::get_v3c4(), Geom::UH_static);
  GeomVertexWriter v(vd, "vertex"), col(vd, "color");
  LPoint3 p[6] = {{0, 0, 2 * s}, {0, 0, 0}, {s, 0, s}, {-s, 0, s}, {0, s, s}, {0, -s, s}};
  for (int i = 0; i < 6; i++) { v.add_data3(p[i]); col.add_data4(i < 2 ? c : c * 0.75f + LColor(0, 0, 0, 0.25f)); }
  PT(GeomTriangles) t = new GeomTriangles(Geom::UH_static);
  int f[8][3] = {{0, 2, 4}, {0, 4, 3}, {0, 3, 5}, {0, 5, 2}, {1, 4, 2}, {1, 3, 4}, {1, 5, 3}, {1, 2, 5}};
  for (auto &q : f) t->add_vertices(q[0], q[1], q[2]);
  PT(Geom) g = new Geom(vd); g->add_primitive(t);
  PT(GeomNode) gn = new GeomNode("marker"); gn->add_geom(g);
  NodePath np(gn); np.set_light_off(1); np.set_two_sided(true);
  return np;
}

static void applyProp(const dna::XmlNode *n, NodePath &target) {
  if (target.is_empty()) return;
  const auto &t = n->tag;
  // A flat wall's pos/scale are the DNANode defaults. Its real place and size are the stacked
  // height and the building width, set below. Letting pos/scale through drops every story on the ground.
  if ((t == "pos" || t == "scale") && target.has_tag("dna_wall")) return;
  if (t == "scale" && target.get_tag("DNARoot") == "baseline") return;
  if (t == "pos") target.set_pos(fl(n->attr("x"), 0), fl(n->attr("y"), 0), fl(n->attr("z"), 0));
  else if (t == "hpr" || t == "nhpr") target.set_hpr(fl(n->attr("h"), 0), fl(n->attr("p"), 0), fl(n->attr("r"), 0));
  else if (t == "scale") {
    auto safe = [&](const char *axis) { double value = fl(n->attr(axis), 1); return value == 0 ? 1e-3 : value; };
    target.set_scale(safe("x"), safe("y"), safe("z"));
  }
  else if (t == "color") target.set_color_scale(fl(n->attr("r"), 1), fl(n->attr("g"), 1), fl(n->attr("b"), 1), fl(n->attr("a"), 1));
}
static bool isProp(const std::string &t) { return t == "pos" || t == "hpr" || t == "nhpr" || t == "scale" || t == "color"; }
static bool hasEditableDesc(const dna::XmlNode *n) {
  bool f = false; dna::walk(n, [&](const dna::XmlNode *c) { if (dna::kEditable.count(c->tag)) f = true; }); return f;
}

// Sign fonts come from the store_font entries of the loaded storage files, exactly as the game
// resolves them. A built-in path is used only for codes no storage declares, and always with a warning.
static PT(TextFont) signFont(const std::string &code, assets::Storage &st) {
  if (code.empty()) return TextNode::get_default_font();
  static std::set<std::string> warned;
  auto warnOnce = [](const std::string &m) { if (warned.insert(m).second) core::warn(m); };
  auto lastResort = []() -> PT(TextFont) {
    PT(TextFont) f = FontPool::load_font("phase_3/fonts/MickeyFont.bam");
    if (f && f->is_valid()) return f;
    return TextNode::get_default_font();
  };
  std::string declared = st.fontPath(code);
  if (!declared.empty()) {
    // Storage paths may omit the extension ("phase_3/fonts/MickeyFont"); the game then loads it
    // with its default model extension (.bam in Clash, .egg in stock Panda3D), so try both.
    std::vector<std::string> candidates = {declared};
    if (Filename(declared).get_extension().empty()) { candidates.push_back(declared + ".bam"); candidates.push_back(declared + ".egg"); }
    for (auto &path : candidates) {
      PT(TextFont) f = FontPool::load_font(path);
      if (f && f->is_valid()) return f;
    }
    warnOnce("font code '" + code + "' is declared as " + declared + " in storage, but that file could not be loaded; using the last-resort font");
    return lastResort();
  }
  std::string path = dna::fallbackSignFontPath(code);
  if (path.empty()) {
    warnOnce("font code '" + code + "' has no store_font entry in the loaded storage and no built-in mapping; using the last-resort font");
    return lastResort();
  }
  warnOnce("font code '" + code + "' has no store_font entry in the loaded storage; using built-in " + path);
  PT(TextFont) f = FontPool::load_font(path);
  if (f && f->is_valid()) return f;
  warnOnce("built-in font " + path + " for code '" + code + "' could not be loaded; using the last-resort font");
  return lastResort();
}

// Baseline dimensions are diameters in world units. Glyph scales belong to the
// letters, while centering and curvature belong to the unscaled baseline.
static void typesetBaseline(NodePath np, const dna::XmlNode *n, const std::string &src, assets::Storage &st) {
  const float deg = float(M_PI) / 180.f;
  const float width = fl(n->attr("width"), 0), height = fl(n->attr("height"), 0);
  const bool curved = width != 0 || height != 0, inner = width < 0 || height < 0;
  const float kern = fl(n->attr("kern"), 0), stumble = fl(n->attr("stumble"), 0);
  const float stomp = fl(n->attr("stomp"), 0), wiggle = fl(n->attr("wiggle"), 0);
  const float indent = fl(n->attr("indent"), 0);
  const auto flags = n->attr("flags");
  LVecBase3 scale(1, 1, 1);
  if (auto c = n->child("scale")) scale.set(fl(c->attr("x"), 1), fl(c->attr("y"), 1), fl(c->attr("z"), 1));
  // Baseline scale sizes the glyphs, not the curve or the baseline position.
  NodePath root = np.attach_new_node("typesetter");
  std::vector<NodePath> glyphs;
  float cursor = 0, priorCursor = 0, totalWidth = 0;
  int counter = 0;
  bool wordStart = true;
  auto add = [&](const dna::XmlNode *entry, const std::string &letter) {
    NodePath glyph;
    float glyphWidth = 0;
    float bigger = flags.find('b') != std::string::npos && wordStart && letter != " " ? 1.5f : 1.f;
    wordStart = letter == " ";
    if (entry->tag == "graphic") {
      NodePath model = st.findNode(entry->attr("code"));
      if (model.is_empty()) return;
      glyph = model.copy_to(root);
      LPoint3 lo, hi;
      if (glyph.calc_tight_bounds(lo, hi)) glyphWidth = hi[0] - lo[0];
      glyphWidth = fl(entry->attr("width"), glyphWidth);
    } else {
      PT(TextNode) tn = new TextNode("glyph");
      PT(TextFont) font = signFont(entry->attr("code", n->attr("code")), st);
      if (font && font->is_valid()) tn->set_font(font);
      tn->set_text(letter);
      if (flags.find('c') != std::string::npos) {
        auto text = tn->get_wtext(); for (auto &ch : text) ch = TextEncoder::unicode_toupper(ch); tn->set_wtext(text);
      }
      if (flags.find('d') != std::string::npos) tn->set_shadow(0.03f, 0.03f);
      glyphWidth = tn->get_width();
      glyph = root.attach_new_node(tn);
    }
    LVector3 pos(0, 0, 0), hpr(0, 0, 0), letterScale(1, 1, 1);
    if (auto c = entry->child("pos")) pos.set(fl(c->attr("x"), 0), fl(c->attr("y"), 0), fl(c->attr("z"), 0));
    auto rot = entry->child("nhpr"); if (!rot) rot = entry->child("hpr");
    if (rot) hpr.set(fl(rot->attr("h"), 0), fl(rot->attr("p"), 0), fl(rot->attr("r"), 0));
    if (auto c = entry->child("scale")) letterScale.set(fl(c->attr("x"), 1), fl(c->attr("y"), 1), fl(c->attr("z"), 1));
    letterScale.set(letterScale[0] * scale[0] * bigger, letterScale[1] * scale[1], letterScale[2] * scale[2] * bigger);
    float advance = glyphWidth * letterScale[0], alternating = counter & 1 ? 1.f : -1.f;
    if (curved) {
      float a = width / 2, b = height / 2;
      // A one-axis curve is legal. Keep the radial division finite at its tip.
      float offset = std::fabs(a) > 1e-5f ? (width < 0 ? pos[0] : -pos[0]) / a : 0;
      float indentation = (width < 0 ? -indent : indent) * deg;
      float angle = cursor + float(M_PI) / 2 + indentation + offset;
      float delta = (pos[2] + alternating * stomp) * (width < 0 ? -1.f : 1.f);
      pos[0] = (a + delta) * std::cos(angle);
      pos[2] = (b + delta) * std::sin(angle);
      hpr[2] -= cursor / deg + indentation / deg + alternating * wiggle;
      float radius = std::max(std::hypot(pos[0], pos[2]), 1e-5f);
      if (width < 0) priorCursor = cursor;
      float next = cursor - 2 * std::asin(std::clamp(advance / (2 * radius), -1.f, 1.f));
      if (width >= 0) priorCursor = next;
      next -= 2 * std::asin(std::clamp((kern - alternating * stumble) / (2 * radius), -1.f, 1.f));
      if (width >= 0) hpr[2] -= (next - cursor) / (2 * deg);
      cursor = next;
    } else {
      pos[0] += totalWidth + counter * kern + alternating * stumble;
      pos[2] += alternating * stomp;
      hpr[2] -= alternating * wiggle;
      totalWidth += advance;
    }
    glyph.set_pos_hpr_scale(pos, hpr, letterScale);
    if (auto c = entry->child("color")) applyProp(c, glyph);
    glyph.set_two_sided(true); glyph.set_depth_write(false); glyph.set_depth_offset(70);
    glyphs.push_back(glyph); ++counter;
  };
  std::vector<const dna::XmlNode *> entries;
  for (auto &c : n->children) if (c->tag == "text" || c->tag == "graphic") entries.push_back(c.get());
  if (inner) std::reverse(entries.begin(), entries.end());
  for (auto entry : entries) {
    if (entry->tag == "graphic") { add(entry, ""); continue; }
    // UTF-8 code points stay together; a merged PDNA line and per-letter DNA
    // must produce identical glyphs and curvature.
    std::string text = dna::nodeText(entry, src);
    std::vector<std::string> letters;
    for (size_t i = 0; i < text.size();) {
      size_t end = i + 1;
      while (end < text.size() && ((unsigned char)text[end] & 0xc0) == 0x80) ++end;
      letters.push_back(text.substr(i, end - i)); i = end;
    }
    if (inner) std::reverse(letters.begin(), letters.end());
    for (auto &letter : letters) add(entry, letter);
  }
  if (curved) {
    root.set_z(-height / 2);
    root.set_r(priorCursor / (2 * deg));
  } else if (counter) {
    float finalGap = (counter - 1) * kern + ((counter - 1) & 1 ? stumble : -stumble);
    root.set_x(-(totalWidth + finalGap) / 2);
  }
}

void Scene::gen(const dna::XmlNode *n, NodePath parent, const std::string &src, assets::Storage &st,
                const std::map<const dna::XmlNode *, int> *ids, float *wallH, const dna::XmlNode *flat) {
  const std::string &t = n->tag;
  if (isProp(t)) { applyProp(n, parent); return; }
  NodePath np; bool recurse = true; float myWallH = 0; const dna::XmlNode *myFlat = flat; float *childWallH = wallH;
  if (t == "group" || t == "node" || t == "visgroup") {
    np = parent.attach_new_node(t == "visgroup" ? "vg_" + n->attr("zone") : n->attr("name", t));
    if (t == "visgroup") visgroupNodes[n->attr("zone")] = np;
  } else if (t == "prop" || t == "anim_prop" || t == "interactive_prop") {
    // Appearance Override: model= path and/or parent_code ATTACH-TO-MODEL (local pose under host).
    std::string code = n->attr("code");
    std::string model = n->attr("model");
    std::string parentCode = n->attr("parent_code");
    NodePath host = parent;
    if (!parentCode.empty()) {
      auto pit = attachParents_.find(parentCode);
      if (pit != attachParents_.end() && !pit->second.is_empty()) host = pit->second;
      else host = ensureAttachParent(parentCode, st, nullptr);
    }
    NodePath src;
    bool isCard = n->attr("card") == "1" || n->attr("card") == "true" ||
                  (!model.empty() && (model.size() > 4 &&
                   (model.compare(model.size() - 4, 4, ".png") == 0 ||
                    model.compare(model.size() - 4, 4, ".PNG") == 0 ||
                    model.compare(model.size() - 4, 4, ".jpg") == 0)));
    if (isCard && !model.empty()) {
      bool bb = n->attr("billboard") == "1" || n->attr("billboard") == "true";
      NodePath card = makeTexturedCard(model, n->attr("name", "prop"), bb);
      np = host.attach_new_node(n->attr("name", "prop"));
      card.reparent_to(np);
    } else {
      // model= with an unknown code (AODNA adds of BAMs no hood storage loads): load the BAM
      // directly instead of warning; a known code still wins, matching the game's resolve order.
      if (!code.empty() && (model.empty() || st.hasCode(code))) src = st.findNode(code);
      if (src.is_empty() && !model.empty()) src = assets::ModelCache::get().load(model);
      np = src.is_empty() ? host.attach_new_node(n->attr("name", "prop")) : src.copy_to(host);
      if (src.is_empty() && code != "DCS") {
        NodePath m = makeMarker(LColor(1, 0.2, 0.8, 1), 1.5f); m.reparent_to(np);
      }
    }
    np.set_name(n->attr("name", "prop"));
  } else if (t == "street") {
    NodePath s = st.findNode(n->attr("code"));
    np = s.is_empty() ? parent.attach_new_node(n->attr("name")) : s.copy_to(parent);
    const char *names[3] = {"street", "sidewalk", "curb"}; int k = 0;
    for (auto &c : n->children) if (c->tag == "texture" && k < 3) {
      PT(Texture) tx = st.findTexture(c->text(src));
      NodePath tn = np.find(std::string("**/*_") + names[k]);
      if (tx && !tn.is_empty()) tn.set_texture(tx, 1);
      k++;
    }
  } else if (t == "flat_building") {
    np = parent.attach_new_node(new ModelNode(n->attr("id", n->attr("name", "flat_building"))));
    myFlat = n; childWallH = &myWallH;
  } else if (t == "wall") {
    if (!flat || !wallH) return;
    std::string code = n->attr("code"); double width = fl(flat->attr("width"), 0), h = fl(n->attr("height"), 0);
    NodePath w = st.findNode(code);
    if (w.is_empty()) { *wallH += h; return; }
    np = w.copy_to(parent);
    np.set_tag("dna_wall", "1");
    double sy = 1, extraZ = 0;
    for (auto &c : n->children) {
      if (c->tag == "scale") sy = fl(c->attr("y"), 1);
      else if (c->tag == "pos") extraZ = fl(c->attr("z"), 0);
    }
    np.set_scale(std::max(width, 0.01), sy == 0 ? 1 : sy, std::max(h, 0.01));
    if (code.rfind("wall_pp_cog_", 0) == 0) {   // PPCogWalls: tiled 10 ft panels instead of one stretched picture
      const double TILE = 10.0; auto ts = TextureStage::get_default();
      np.set_tex_scale(ts, width / TILE, h / TILE); np.set_tex_offset(ts, 0, fmod(*wallH, TILE) / TILE);
    } else if (code.rfind("wall_pp_bay", 0) == 0) {
      static bool once = false; if (!once) { once = true; core::info("note: Bandwidth 3D office walls (wall_pp_bay*) are shown as plain stretched walls in the editor"); }
    }
    np.set_z(*wallH + extraZ); *wallH += h;
  } else if (t == "windows") {
    NodePath w = st.findNode(n->attr("code")); int count = (int)fl(n->attr("count"), 0);
    if (!w.is_empty()) {
      NodePath ws = parent.attach_new_node("windows"); ws.set_depth_offset(3);
      for (int i = 0; i < count; i++) {
        NodePath c = w.copy_to(ws); c.set_scale(c.get_top(), 1, 1, 1); c.set_x((i + 1) / (count + 1.0)); c.set_z(0.5);
      }
    }
    return;   // DNAWindows returns no node: its children are not generated
  } else if (t == "cornice") {
    NodePath c = st.findNode(n->attr("code")); if (c.is_empty()) return;
    np = c.copy_to(parent); np.set_z(1); np.set_depth_offset(1);
  } else if (t == "flat_door") {
    NodePath c = st.findNode(n->attr("code")); if (c.is_empty()) return;
    np = c.copy_to(parent); np.set_scale(np.get_top(), 1, 1, 1); np.set_pos(0.5, 0, 0); np.set_depth_offset(1);
  } else if (t == "landmark_building" || t == "anim_building") {
    NodePath s = st.findNode(n->attr("code"));
    np = s.is_empty() ? parent.attach_new_node(n->attr("id")) : s.copy_to(parent);
    np.set_name(n->attr("id", "landmark"));
    NodePath o = np.find("**/*suit_building_origin"); if (!o.is_empty()) o.hide();
  } else if (t == "door") {
    NodePath d = st.findNode(n->attr("code")); if (d.is_empty()) return;
    NodePath origin = parent.find("**/*door_origin");
    np = d.copy_to(parent);
    if (!origin.is_empty()) np.set_transform(origin.get_transform(parent));
  } else if (t == "treasure_spawn") {
    np = parent.attach_new_node("treasure_spawn");
    NodePath m = makeMarker(LColor(1.0f, 0.85f, 0.1f, 1), 1.2f); m.reparent_to(np);
  } else if (t == "sign") {   // placed at the building's sign_origin; the sign's own pos/hpr/scale are relative to it
    NodePath origin = parent.find("**/sign_origin");   // look up before adding our own node
    if (origin.is_empty()) origin = parent.find("**/*sign_origin*");
    NodePath holder = parent.attach_new_node("dna_sign");
    if (!origin.is_empty()) holder.set_transform(origin.get_transform(parent));
    NodePath b = st.findNode(n->attr("code"));
    np = b.is_empty() ? holder.attach_new_node("sign") : b.copy_to(holder);
    np.set_depth_offset(50);
  } else if (t == "baseline") {
    np = parent.attach_new_node("baseline");
    for (auto &c : n->children) if (c->tag == "text_geom") {   // older .pdna: pre-rendered text model (zlib'd BAM)
      std::string raw = decompress_string(unb64(c->text(src)));
      if (raw.empty()) continue;
      // These are complete BAM files, not NodePath.encode_to_bam_stream()
      // payloads: the latter has an additional path/error datagram.
      std::istringstream stream(raw, std::ios::in | std::ios::binary);
      BamFile bam;
      NodePath g;
      if (bam.open_read(stream, "sign text")) {
        PT(PandaNode) model = bam.read_node();
        if (model) g = NodePath(model);
      }
      if (!g.is_empty()) { g.reparent_to(np); g.set_two_sided(true); g.set_depth_offset(60); }
    }
    if (!n->child("text_geom")) typesetBaseline(np, n, src, st);
  } else if (t == "text_geom" || t == "text" || t == "graphic" || t == "title" || t == "texture" ||
             t == "battle_cell" || t == "suit_edge" || t == "store_suit_point" || t == "curve_point" ||
             t == "prop_list") {
    return;   // gameplay data or sign text: not drawn in the editor
  } else if (t == "scene" || t == "#doc") {
    np = parent;
  } else {
    if (unknownTags_[t]++ == 0) core::info("editor does not draw <" + t + "> elements (skipped)");
    return;
  }
  if (np.is_empty()) return;
  if (t == "anim_prop" || t == "interactive_prop" || t == "anim_building") {
    auto controls = assets::loopModel(np, st.modelPath(n->attr("code")), n->attr("anim"));
    animations.insert(animations.end(), controls.begin(), controls.end());
  }
  stats_nodes++;
  np.set_tag("DNARoot", t);
  int eid = -1;
  if (ids) { auto it = ids->find(n); if (it != ids->end()) eid = it->second; }
  if (eid >= 0) { np.set_tag("eid", std::to_string(eid)); if ((int)itemNodes.size() > eid) itemNodes[eid] = np; }
  if (recurse) for (auto &c : n->children) gen(c.get(), np, src, st, ids, childWallH, myFlat);
  if (eid >= 0 && t != "treasure_spawn" && !hasEditableDesc(n)) {
    // Flatten below the item (its own transform stays editable): far fewer nodes to draw.
    { NodePathCollection ch = np.get_children(); for (int k = 0; k < ch.get_num_paths(); k++) if (!ch.get_path(k).node()->is_collision_node()) ch.get_path(k).flatten_strong(); }
  }
}

void Scene::build(const dna::Document &doc, assets::Storage &st) {
  clear();
  itemNodes.assign(doc.items.size(), NodePath());
  prepareAttachParents(doc, st);
  std::map<const dna::XmlNode *, int> ids;
  for (size_t i = 0; i < doc.items.size(); i++) if (doc.items[i].node) ids[doc.items[i].node] = (int)i;
  for (auto &c : doc.root->children) gen(c.get(), geom, doc.text, st, &ids, nullptr, nullptr);
  { auto cc = geom.find_all_matches("**/+CollisionNode"); cc.hide(); }
  for (size_t i = 0; i < doc.items.size(); i++) {
    if (itemNodes[i].is_empty()) {   // e.g. elements inside an undrawn parent: keep a marker so it can be edited
      NodePath p = markers.attach_new_node("item"); makeMarker(LColor(0.5, 0.8, 1, 1), 1.0f).reparent_to(p);
      p.set_tag("eid", std::to_string(i)); itemNodes[i] = p; syncItem(doc, (int)i);
    }
  }
  computeBounds();
}

void Scene::buildAdded(const dna::Document &doc, int i, assets::Storage &st) {
  if ((int)itemNodes.size() <= i) itemNodes.resize(i + 1);
  const auto &it = doc.items[i];
  auto x = dna::parseXml(it.templateXml);
  NodePath host = visgroupNodes.count(it.visgroup) ? visgroupNodes[it.visgroup] : (visgroupNodes.empty() ? geom : visgroupNodes.begin()->second);
  if (x->children.empty()) return;
  std::map<const dna::XmlNode *, int> ids{{x->children[0].get(), i}};
  gen(x->children[0].get(), host, it.templateXml, st, &ids, nullptr, nullptr);
  if (itemNodes[i].is_empty()) { NodePath p = host.attach_new_node("item"); makeMarker(LColor(0.5, 0.8, 1, 1), 1).reparent_to(p); p.set_tag("eid", std::to_string(i)); itemNodes[i] = p; }
  { auto cc = itemNodes[i].find_all_matches("**/+CollisionNode"); cc.hide(); }
  syncItem(doc, i);
}

void Scene::syncItem(const dna::Document &doc, int i) {
  if (i < 0 || i >= (int)itemNodes.size() || itemNodes[i].is_empty()) return;
  const auto &it = doc.items[i]; NodePath np = itemNodes[i];
  const auto &x = it.xf;
  np.set_pos_hpr_scale(x.x, x.y, x.z, x.h, x.p, x.r, x.sx == 0 ? 1e-3 : x.sx, x.sy == 0 ? 1e-3 : x.sy, x.sz == 0 ? 1e-3 : x.sz);
  if (it.deleted) np.stash(); else np.unstash();
}

void Scene::setVisgroupVisible(const std::string &z, bool v) {
  auto it = visgroupNodes.find(z); if (it == visgroupNodes.end()) return;
  if (v) it->second.show(); else it->second.hide();
}

void Scene::computeBounds() {
  LPoint3 a, b;
  if (geom.calc_tight_bounds(a, b) && (b - a).length() < 1e6) { boundsMin = a; boundsMax = b; }
  else { boundsMin = LPoint3(-50, -50, 0); boundsMax = LPoint3(50, 50, 10); }
}

}  // namespace render
