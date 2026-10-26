#include "scene.h"
#include "core/log.h"
#include "geomNode.h"
#include "geomTriangles.h"
#include "geomVertexWriter.h"
#include "modelNode.h"
#include "textureStage.h"
#include "depthOffsetAttrib.h"
#include "collisionNode.h"
#include "compress_string.h"
#include "textNode.h"
#include <set>

namespace render {

static std::string unb64(const std::string &in) {
  std::string o; int v = 0, b = -8;
  for (unsigned char c : in) {
    int d = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52 : c == '+' ? 62 : c == '/' ? 63 : -1;
    if (d < 0) continue; v = (v << 6) + d; b += 6; if (b >= 0) { o += char((v >> b) & 255); b -= 8; }
  }
  return o;
}
static double fl(const std::string &s, double d) { if (s.empty()) return d; char *e; double v = strtod(s.c_str(), &e); return e == s.c_str() ? d : v; }

Scene::Scene() { world = NodePath("world"); geom = world.attach_new_node("dna"); markers = world.attach_new_node("markers"); }

void Scene::clear() {
  geom.remove_node(); markers.remove_node();
  geom = world.attach_new_node("dna"); markers = world.attach_new_node("markers");
  itemNodes.clear(); visgroupNodes.clear(); unknownTags_.clear(); stats_nodes = 0;
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
  if (t == "pos") target.set_pos(fl(n->attr("x"), 0), fl(n->attr("y"), 0), fl(n->attr("z"), 0));
  else if (t == "hpr" || t == "nhpr") target.set_hpr(fl(n->attr("h"), 0), fl(n->attr("p"), 0), fl(n->attr("r"), 0));
  else if (t == "scale") target.set_scale(fl(n->attr("x"), 1), fl(n->attr("y"), 1), fl(n->attr("z"), 1));
  else if (t == "color") target.set_color_scale(fl(n->attr("r"), 1), fl(n->attr("g"), 1), fl(n->attr("b"), 1), fl(n->attr("a"), 1));
}
static bool isProp(const std::string &t) { return t == "pos" || t == "hpr" || t == "nhpr" || t == "scale" || t == "color"; }
static bool hasEditableDesc(const dna::XmlNode *n) {
  bool f = false; dna::walk(n, [&](const dna::XmlNode *c) { if (dna::kEditable.count(c->tag)) f = true; }); return f;
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
    NodePath src = st.findNode(n->attr("code"));
    np = src.is_empty() ? parent.attach_new_node(n->attr("name", "prop")) : src.copy_to(parent);
    if (src.is_empty() && n->attr("code") != "DCS") { NodePath m = makeMarker(LColor(1, 0.2, 0.8, 1), 1.5f); m.reparent_to(np); }
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
    np.set_scale(std::max(width, 0.01), 1, std::max(h, 0.01));
    if (code.rfind("wall_pp_cog_", 0) == 0) {   // PPCogWalls: tiled 10 ft panels instead of one stretched picture
      const double TILE = 10.0; auto ts = TextureStage::get_default();
      np.set_tex_scale(ts, width / TILE, h / TILE); np.set_tex_offset(ts, 0, fmod(*wallH, TILE) / TILE);
    } else if (code.rfind("wall_pp_bay", 0) == 0) {
      static bool once = false; if (!once) { once = true; core::info("note: Bandwidth 3D office walls (wall_pp_bay*) are shown as plain stretched walls in the editor"); }
    }
    np.set_z(*wallH); *wallH += h;
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
      vector_uchar v; v.insert(v.end(), (const unsigned char *)raw.data(), (const unsigned char *)raw.data() + raw.size());
      NodePath g = NodePath::decode_from_bam_stream(std::move(v));
      if (!g.is_empty()) { g.reparent_to(np); g.set_two_sided(true); g.set_depth_offset(60); }
    }
    if (np.get_num_children() == 0) {   // .dna/.xml: draw the letters with a TextNode (straight line, default font)
      std::string letters; double kern = 0; LColor col(0, 0, 0, 1);
      for (auto &c : n->children) {
        if (c->tag == "kern") kern = fl(c->text(src), 0);
        if (c->tag == "color") col = LColor(fl(c->attr("r"), 0), fl(c->attr("g"), 0), fl(c->attr("b"), 0), fl(c->attr("a"), 1));
        if (c->tag == "text") for (auto &k : c->children) if (k->tag == "letters") letters += k->text(src);
      }
      if (!letters.empty()) {
        PT(TextNode) tn = new TextNode("sign_text"); tn->set_text(letters); tn->set_align(TextNode::A_center);
        tn->set_text_color(1, 1, 1, 1); (void)col;   // the baseline colour arrives as a colour scale
        NodePath tp = np.attach_new_node(tn); tp.set_two_sided(true); tp.set_depth_offset(60); tp.set_y(-0.05f);
        (void)kern;
      }
    }
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
