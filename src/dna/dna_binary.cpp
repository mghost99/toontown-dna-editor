// Compiled DNA (.pdna). Layout follows libpandadna's DNALoader (reader) / compiler (writer): written here from
// the format, no code copied. All integers little endian; strings = uint16 length + bytes.
//   "PDNA\n" <uint8 compressed> "\n" then (zlib if compressed):
//   storage: catalog{u16 roots: str root, u8 n, str code*n} textures{u16: str code, str file}
//            fonts{u16: code, file} nodes/hood_nodes/place_nodes{u16: code, file, search}
//            blocks{u16: u8 num, u16 zone, str title, str article, str type}
//            suit_points{u16: u16 idx, u8 type, i32 x*100,y,z, i16 landmark}
//            suit_edges{u16: u16 start, u16 n, (u16 end, u16 zone)*n}
//   components: u8 code + fields [+ children + 255]; windows/cornice/door/flat_door/street have no children.
#include "dna_formats.h"
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>

namespace dna {

static ZFn g_inflate, g_deflate;
void setZlib(ZFn inflate, ZFn deflate) { g_inflate = std::move(inflate); g_deflate = std::move(deflate); }
bool haveZlib() { return (bool)g_inflate && (bool)g_deflate; }

enum : uint8_t { C_GROUP = 1, C_VISGROUP, C_NODE, C_PROP, C_SIGN, C_BASELINE, C_TEXT, C_GRAPHIC, C_FLAT_BUILDING, C_WALL,
                 C_WINDOWS, C_CORNICE, C_LANDMARK, C_ANIM_PROP, C_INTERACTIVE_PROP, C_ANIM_BUILDING, C_DOOR, C_FLAT_DOOR,
                 C_STREET, C_RETURN = 255 };
static const char *kPointTypes[] = {"STREET_POINT", "FRONT_DOOR_POINT", "SIDE_DOOR_POINT", "COGHQ_IN_POINT", "COGHQ_OUT_POINT"};

// ---------------------------------------------------------------- reader
namespace {
struct R {
  // old = the older compiled layout (2016-era fan-server .pdna): 1-byte string lengths, a global battle-cell
  // table after the suit edges, and every component followed by its parent's and visgroup's names.
  // wide = Corporate Clash: pos/hpr, suit points and battle cells are float64 (scale stays 0.01).
  // merged = Clash and Toontown Stride: a baseline stores the whole sign line, then the font; landmark
  // titles live only in the storage block table.
  const std::string &d; size_t p = 0; bool bad = false; bool old = false; bool wide = false; bool merged = false;
  explicit R(const std::string &s, size_t at, bool o = false, bool w = false, bool m = false)
    : d(s), p(at), old(o), wide(w), merged(m) {}
  bool need(size_t n) { if (p + n > d.size()) { bad = true; return false; } return true; }
  uint64_t le(int n) { if (!need(n)) return 0; uint64_t v = 0; for (int i = 0; i < n; i++) v |= (uint64_t)(uint8_t)d[p + i] << (8 * i); p += n; return v; }
  uint8_t u8() { return (uint8_t)le(1); }
  uint16_t u16() { return (uint16_t)le(2); }
  int16_t i16() { return (int16_t)le(2); }
  int32_t i32() { return (int32_t)le(4); }
  float f32() { uint32_t u = (uint32_t)le(4); float f; memcpy(&f, &u, 4); return f; }
  double f64() { uint64_t u = le(8); double f; memcpy(&f, &u, 8); return f; }
  std::string str() { uint16_t n = old ? u8() : u16(); if (!need(n)) return ""; std::string s = d.substr(p, n); p += n; return s; }
  size_t left() const { return d.size() - p; }
};

std::string num(double v, int prec) {
  char b[64]; snprintf(b, sizeof b, "%.*f", prec, v);
  std::string s = b;
  if (s.find('.') != std::string::npos) { while (s.back() == '0') s.pop_back(); if (s.back() == '.') s.pop_back(); }
  if (s == "-0") s = "0";
  return s;
}
std::string b64(const std::string &in) {
  static const char *A = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string o; int v = 0, b = -6;
  for (unsigned char c : in) { v = (v << 8) + c; b += 8; while (b >= 0) { o += A[(v >> b) & 63]; b -= 6; } }
  if (b > -6) o += A[((v << 8) >> (b + 8)) & 63];
  while (o.size() % 4) o += '=';
  return o;
}
std::string qs(const std::string &s) { return "\"" + s + "\""; }

struct BlockRec { std::string title, article, type; };
int blockIndex(const std::string &id) {   // "tb12:..." / "sz18:..." -> 12 / 18
  auto c = id.find(':'); if (c == std::string::npos) return -1;
  size_t i = 0; while (i < c && !std::isdigit((unsigned char)id[i])) i++;
  if (i >= c) return -1;
  return std::atoi(id.c_str() + i);
}

struct Dec {
  R &r; std::string out; int depth = 0; std::string *err;
  std::map<int, BlockRec> blocks;
  std::string ind() const { return std::string(depth, '\t'); }
  void line(const std::string &s) { out += ind() + s + "\n"; }
  void color(const char *kw = "color") { double c[4]; for (auto &x : c) x = r.u8() / 255.0; line(std::string(kw) + " [ " + num(c[0], 9) + " " + num(c[1], 9) + " " + num(c[2], 9) + " " + num(c[3], 9) + " ]"); }
  void node() {   // DNANode fields after the name. Clash stores pos/hpr as float64; everyone else uses 0.01 fixed point.
    double v[9];
    int prec = 2;
    if (r.wide) { prec = 6; for (int i = 0; i < 6; i++) v[i] = r.f64(); }
    else { for (int i = 0; i < 6; i++) v[i] = r.i32() / 100.0; }
    for (int i = 6; i < 9; i++) v[i] = r.i16() / 100.0;
    line("pos [ " + num(v[0], prec) + " " + num(v[1], prec) + " " + num(v[2], prec) + " ]");
    line("nhpr [ " + num(v[3], prec) + " " + num(v[4], prec) + " " + num(v[5], prec) + " ]");
    if (v[6] != 1 || v[7] != 1 || v[8] != 1) line("scale [ " + num(v[6], prec) + " " + num(v[7], prec) + " " + num(v[8], prec) + " ]");
  }
  void xyz(double &x, double &y, double &z) {
    if (r.wide) { x = r.f64(); y = r.f64(); z = r.f64(); }
    else { x = r.i32() / 100.0; y = r.i32() / 100.0; z = r.i32() / 100.0; }
  }
  void codeColor() { std::string c = r.str(); if (!c.empty()) line("code [ " + qs(c) + " ]"); color(); }
  // Returns false at the end of the parent's children (255) or on error.
  bool comp() {
    if (!r.left()) return false;
    uint8_t code = r.u8();
    if (code == C_RETURN) return false;
    std::string name = r.str();
    static const char *kw[] = {"", "group", "visgroup", "node", "prop", "sign", "baseline", "text", "graphic", "flat_building",
                               "wall", "windows", "cornice", "landmark_building", "anim_prop", "interactive_prop",
                               "anim_building", "door", "flat_door", "street"};
    if (r.old) { r.str(); r.str(); }   // parent name + visgroup name (the nesting already gives both)
    if (code < 1 || code > C_STREET) { if (err) *err = "invalid component code " + std::to_string(code); r.bad = true; return false; }
    bool named = code == C_GROUP || code == C_VISGROUP || code == C_NODE || code == C_PROP || code == C_FLAT_BUILDING ||
                 code == C_LANDMARK || code == C_ANIM_PROP || code == C_INTERACTIVE_PROP || code == C_ANIM_BUILDING || code == C_STREET;
    line(std::string(kw[code]) + (named ? " " + qs(name) : "") + " [");
    depth++;
    bool leaf = false;
    switch (code) {
      case C_GROUP: break;
      case C_VISGROUP: {
        std::vector<std::pair<int, int>> edges; int ne = r.u16(); for (int i = 0; i < ne && !r.bad; i++) { int a = r.u16(); edges.push_back({a, r.u16()}); }
        int nv = r.u16(); std::string vis; for (int i = 0; i < nv && !r.bad; i++) vis += " " + qs(r.str());
        if (nv) line("vis [" + vis + " ]");
        for (auto &[a, b] : edges) line("suit_edge [ " + std::to_string(a) + " " + std::to_string(b) + " ]");
        int nc = r.u16();
        for (int i = 0; i < nc && !r.bad; i++) { int w = r.u8(), h = r.u8(); double x, y, z; xyz(x, y, z);
          int prec = r.wide ? 6 : 2;
          line("battle_cell [ " + std::to_string(w) + " " + std::to_string(h) + " " + num(x, prec) + " " + num(y, prec) + " " + num(z, prec) + " ]"); }
        break;
      }
      case C_NODE: node(); break;
      case C_PROP: case C_ANIM_PROP: case C_INTERACTIVE_PROP: case C_SIGN: {
        node(); codeColor();
        if (code == C_ANIM_PROP || code == C_INTERACTIVE_PROP) { std::string a = r.str(); line("anim [ " + qs(a) + " ]"); }
        if (code == C_INTERACTIVE_PROP) line("cell_id [ " + std::to_string(r.i16()) + " ]");
        break;
      }
      case C_GRAPHIC: {
        node(); std::string cd = r.str(); size_t at = out.size(); color(); std::string col = out.substr(at); out.resize(at);
        double w = r.i16() / 100.0, h = r.i16() / 100.0; bool dflt = r.u8() != 0;
        if (!cd.empty()) line("code [ " + qs(cd) + " ]");
        if (!dflt) out += col;   // colour only when the source set one
        line("width [ " + num(w, 2) + " ]"); line("height [ " + num(h, 2) + " ]"); break;
      }
      case C_BASELINE: {
        if (r.old) {   // older files: node + u16 length + zlib'd BAM of the pre-rendered sign text (letters not stored)
          node(); int n = r.u16(); std::string blob; if (r.need(n)) { blob = r.d.substr(r.p, n); r.p += n; }
          line("// sign text was stored as pre-rendered geometry; letters not recoverable (drawn from text_geom)");
          if (!blob.empty()) line("text_geom [ " + qs(b64(blob)) + " ]");
          break;
        }
        node();
        std::string letters;
        if (r.merged) letters = r.str();   // the whole sign line, ahead of the font code
        codeColor(); std::string fl = r.str(); line("flags [ " + qs(fl) + " ]");
        const char *k[] = {"indent", "kern", "wiggle", "stumble", "stomp", "width", "height"};
        for (auto kk : k) { char b[32]; snprintf(b, sizeof b, "%.9g", (double)r.f32()); line(std::string(kk) + " [ " + b + " ]"); }   // exact f32
        if (!letters.empty()) { line("text ["); depth++; line("letters [ " + qs(letters) + " ]"); depth--; line("]"); }
        break;
      }
      case C_TEXT: { node(); std::string l = r.str(); line("letters [ " + qs(l) + " ]"); codeColor(); break; }
      case C_FLAT_BUILDING: { node(); double w = r.u16() / (r.old ? 100.0 : 10.0); r.u8(); line("width [ " + num(w, r.old ? 2 : 1) + " ]"); break; }   // older files: width in 0.01
      case C_WALL: { node(); std::string c = r.str(); double h = r.i16() / 100.0; if (!c.empty()) line("code [ " + qs(c) + " ]"); line("height [ " + num(h, 2) + " ]"); color(); break; }
      case C_WINDOWS: { codeColor(); line("count [ " + std::to_string(r.u8()) + " ]"); leaf = true; break; }
      case C_CORNICE: case C_DOOR: case C_FLAT_DOOR: codeColor(); leaf = true; break;
      case C_LANDMARK: case C_ANIM_BUILDING: {
        node(); std::string c = r.str(); if (!c.empty()) line("code [ " + qs(c) + " ]");
        color();
        std::string t, a, bt;
        if (!r.merged) { t = r.str(); a = r.str(); bt = r.str(); if (code == C_ANIM_BUILDING) { std::string an = r.str(); line("anim [ " + qs(an) + " ]"); } }
        else if (auto it = blocks.find(blockIndex(name)); it != blocks.end()) { t = it->second.title; a = it->second.article; bt = it->second.type; }
        if (!t.empty()) line("title [ " + qs(t) + " ]");
        if (!a.empty()) line("article [ " + qs(a) + " ]");
        if (!bt.empty()) line("building_type [ " + qs(bt) + " ]");
        break;
      }
      case C_STREET: {
        node(); std::string c = r.str(); line("code [ " + qs(c) + " ]");
        for (int i = 0; i < 3; i++) line("texture [ " + qs(r.str()) + " ]");
        for (int i = 0; i < 3; i++) color();
        leaf = true; break;
      }
    }
    if (!leaf) while (!r.bad && comp()) {}
    depth--; line("]");
    return !r.bad;
  }
};
}  // namespace

static bool decompileWith(const std::string &data, bool old, bool wide, bool merged, std::string *text, std::string *err);
bool decompilePdna(const std::string &bytes, std::string *text, std::string *err, bool *compressedOut) {
  if (bytes.size() < 7 || bytes.compare(0, 5, "PDNA\n") != 0) { if (err) *err = "not a compiled DNA file (no PDNA header)"; return false; }
  bool compressed = bytes[5] != 0;
  if (compressedOut) *compressedOut = compressed;
  std::string data = bytes.substr(7);
  if (compressed) {
    if (!g_inflate) { if (err) *err = "compressed .pdna needs zlib (not available in this build)"; return false; }
    std::string raw; if (!g_inflate(data, &raw)) { if (err) *err = "zlib decompression failed"; return false; }
    data.swap(raw);
  }
  if (decompileWith(data, false, false, false, text, err)) return true;          // libpandadna
  std::string eStride, eClash, eOld;
  if (decompileWith(data, false, false, true, text, &eStride)) return true;      // Toontown Stride: sign line on the baseline
  if (decompileWith(data, false, true, true, text, &eClash)) return true;        // Corporate Clash: float64 coordinates too
  if (decompileWith(data, true, false, false, text, &eOld)) return true;         // older compiled layout
  if (err && !eStride.empty()) *err += " (stride layout: " + eStride + ")";
  if (err && !eClash.empty()) *err += " (clash layout: " + eClash + ")";
  if (err && !eOld.empty()) *err += " (older layout: " + eOld + ")";
  return false;
}

static bool decompileWith(const std::string &data, bool old, bool wide, bool merged, std::string *text, std::string *err) {
  R r(data, 0, old, wide, merged);
  std::string o = "// Decompiled from .pdna by the DNA editor. Values are fixed point in the binary format\n"
                  "// (positions/angles/scales 0.01, colours 1/255).\n\n";
  // catalog
  std::map<std::string, std::string> rootOf; std::vector<std::pair<std::string, std::string>> catalog;
  int n = r.u16();
  for (int i = 0; i < n && !r.bad; i++) { std::string root = r.str(); int k = r.u8(); for (int j = 0; j < k && !r.bad; j++) { std::string c = r.str(); catalog.push_back({root, c}); rootOf.emplace(c, root); } }
  std::set<std::string> used;
  std::string tex, fonts;
  n = r.u16(); for (int i = 0; i < n && !r.bad; i++) { std::string c = r.str(), f = r.str(); used.insert(c);
    tex += "store_texture [ " + (rootOf.count(c) ? qs(rootOf[c]) + " " : "") + qs(c) + " " + qs(f) + " ]\n"; }
  n = r.u16(); for (int i = 0; i < n && !r.bad; i++) { std::string c = r.str(), f = r.str(); used.insert(c);
    fonts += "store_font [ " + qs(rootOf.count(c) ? rootOf[c] : "font") + " " + qs(c) + " " + qs(f) + " ]\n"; }
  const char *kinds[] = {"model", "hood_model", "place_model"};
  std::string models;
  for (int kind = 0; kind < 3; kind++) {
    std::vector<std::string> order; std::map<std::string, std::string> body;
    n = r.u16();
    for (int i = 0; i < n && !r.bad; i++) {
      std::string c = r.str(), f = r.str(), s = r.str(); used.insert(c);
      if (!body.count(f)) order.push_back(f);
      body[f] += "\tstore_node [ " + qs(rootOf.count(c) ? rootOf[c] : "prop") + " " + qs(c) + (s != c ? " " + qs(s) : "") + " ]\n";
    }
    for (auto &f : order) models += std::string(kinds[kind]) + " " + qs(f) + " [\n" + body[f] + "]\n";
  }
  std::map<int, BlockRec> blocks;
  n = r.u16();
  for (int i = 0; i < n && !r.bad; i++) {
    int num = r.u8(), zone = r.u16(); std::string title = r.str(), article = r.str(), type = r.str();
    (void)zone; blocks[num] = {title, article, type};
  }
  std::string pts;
  n = r.u16();
  int pprec = wide ? 6 : 2;
  for (int i = 0; i < n && !r.bad; i++) {
    int idx = r.u16(), ty = r.u8(); double x, y, z;
    if (wide) { x = r.f64(); y = r.f64(); z = r.f64(); } else { x = r.i32() / 100.0; y = r.i32() / 100.0; z = r.i32() / 100.0; }
    int lb = r.i16();
    pts += "store_suit_point [ " + std::to_string(idx) + ", " + (ty < 5 ? kPointTypes[ty] : "STREET_POINT") + ", " + num(x, pprec) + " " + num(y, pprec) + " " + num(z, pprec) + (lb != -1 ? ", " + std::to_string(lb) : "") + " ]\n";
  }
  n = r.u16(); for (int i = 0; i < n && !r.bad; i++) { r.u16(); int k = r.u16(); for (int j = 0; j < k && !r.bad; j++) { r.u16(); r.u16(); } }   // edges: in visgroups
  if (r.bad) { if (err) *err = "truncated .pdna storage section"; return false; }
  o += models + tex + fonts + pts;
  // codes only in the catalog (no node/texture/font): keep them so nothing is lost
  for (auto &[root, c] : catalog) if (!used.count(c)) o += "// catalog-only code: " + root + " " + c + "\n";
  if (!o.empty() && o.back() != '\n') o += "\n";
  Dec d{r, "", 0, err, std::move(blocks)};
  int oldCells = 0;
  if (old) { int nc = r.u16(); for (int i = 0; i < nc && !r.bad; i++) { r.u8(); r.u8(); r.i32(); r.i32(); r.i32(); oldCells++; } }
  // the component stream starts with the root group; emit its children at top level
  if (r.left()) {
    uint8_t c = r.u8(); r.str(); if (old) { r.str(); r.str(); }
    if (c != C_GROUP) { if (err) *err = "component stream does not start with the root group"; return false; }
    while (!r.bad && d.comp()) {}
  }
  if (r.bad) { if (err && err->empty()) *err = "truncated .pdna component data"; return false; }
  if (oldCells) o += "// " + std::to_string(oldCells) + " battle cells from the file's global table were not kept\n";
  *text = o + "\n" + d.out;
  return true;
}

// ---------------------------------------------------------------- writer
namespace {
struct W {
  std::string b;
  void le(uint64_t v, int n) { for (int i = 0; i < n; i++) b += (char)((v >> (8 * i)) & 0xff); }
  void u8(int v) { le((uint8_t)v, 1); } void u16(int v) { le((uint16_t)v, 2); } void i16(int v) { le((uint16_t)(int16_t)v, 2); }
  void i32(long long v) { le((uint32_t)(int32_t)v, 4); }
  void f32(double v) { float f = (float)v; uint32_t u; memcpy(&u, &f, 4); le(u, 4); }
  void str(const std::string &s) { u16((int)s.size()); b += s.substr(0, 65535); }
};
double fl(const std::string &s, double d) { if (s.empty()) return d; char *e; double v = strtod(s.c_str(), &e); return e == s.c_str() ? d : v; }
// Fixed point like the reference compiler (int(v*100), truncation) but with a 1e-6 nudge toward the nearest
// integer, so values like 0.29 (28.999999...) store as 29 and decompile -> recompile is stable.
long long trunc6(double v) { double r = std::round(v); return (long long)(std::fabs(v - r) < 1e-6 ? r : v); }
long long fx(double v) { return trunc6(v * 100); }

template <class K, class V> struct Ordered {   // insertion-ordered map with overwrite (python dict semantics)
  std::vector<K> keys; std::map<K, V> m;
  V &operator[](const K &k) { if (!m.count(k)) keys.push_back(k); return m[k]; }
};

struct Comp {
  const std::string &src; W w; CompileStats st; std::set<std::string> skipped;
  // storage
  Ordered<std::string, std::vector<std::string>> catalog;
  Ordered<std::string, std::string> textures, fonts;
  Ordered<std::string, std::pair<std::string, std::string>> nodes[3];
  struct Block { int zone = 0; std::string title, article, type; };
  Ordered<int, Block> blocks;
  struct Pt { int idx, type; double x, y, z; int lb; };
  std::vector<Pt> points;
  Ordered<int, std::vector<std::pair<int, int>>> edges;

  explicit Comp(const std::string &s) : src(s) {}
  static const XmlNode *kid(const XmlNode *n, const char *t, int k = 0) {
    for (auto &c : n->children) if (c->tag == t && k-- == 0) return c.get();
    return nullptr;
  }
  void colorOf(const XmlNode *c) {
    for (const char *k : {"r", "g", "b", "a"}) w.u8((int)trunc6((c ? fl(c->attr(k), 1) : 1.0) * 255));
  }
  void node(const XmlNode *n) {
    const XmlNode *p = kid(n, "pos"), *h = kid(n, "nhpr"), *s = kid(n, "scale");
    if (!h) h = kid(n, "hpr");
    for (const char *k : {"x", "y", "z"}) w.i32(fx(p ? fl(p->attr(k), 0) : 0));
    for (const char *k : {"h", "p", "r"}) w.i32(fx(h ? fl(h->attr(k), 0) : 0));
    for (const char *k : {"x", "y", "z"}) w.i16((int)fx(s ? fl(s->attr(k), 1) : 1));
  }
  static int blockOf(const std::string &id) {   // "tb12:..." -> 12
    size_t c = id.find(':'); if (id.size() < 3 || c == std::string::npos || c < 3) return -1;
    return atoi(id.substr(2, c - 2).c_str());
  }
  void storage(const XmlNode *n) {
    const std::string &t = n->tag;
    if (t == "store_node") {
      std::string root = n->attr("root"), code = n->attr("code");
      std::string search = n->attrs.count("node") ? n->attr("node") : code;
      catalog[root].push_back(code);
      std::string file = n->parent ? n->parent->attr("path") : "", scope = n->parent ? n->parent->attr("scope") : "";
      size_t sl = file.find_last_of('/'), dot = file.find_last_of('.');
      if (dot == std::string::npos || (sl != std::string::npos && dot < sl)) file += ".bam";
      nodes[scope == "hood" ? 1 : scope == "place" ? 2 : 0][code] = {file, search};
    } else if (t == "store_texture") {
      if (n->attrs.count("root")) catalog[n->attr("root")].push_back(n->attr("code"));
      textures[n->attr("code")] = n->attr("path");
    } else if (t == "store_font") {
      std::string f = n->attr("path"); size_t sl = f.find_last_of('/'), dot = f.find_last_of('.');
      if (dot == std::string::npos || (sl != std::string::npos && dot < sl)) f += ".bam";
      catalog[n->attr("root")].push_back(n->attr("code")); fonts[n->attr("code")] = f;
    } else if (t == "store_suit_point") {
      std::string ty = n->attr("type"); int k = 0; for (int i = 0; i < 5; i++) if (ty == kPointTypes[i]) k = i;
      points.push_back({atoi(n->attr("id").c_str()), k, fl(n->attr("x"), 0), fl(n->attr("y"), 0), fl(n->attr("z"), 0),
                        n->attrs.count("building") ? atoi(n->attr("building").c_str()) : -1});
    }
  }
  std::string zoneOf(const XmlNode *n) {
    for (auto p = n->parent; p; p = p->parent) if (p->tag == "visgroup") return p->attr("zone");
    return "";
  }
  void children(const XmlNode *n) {
    for (auto &c : n->children) comp(c.get());
    w.u8(C_RETURN);
  }
  void comp(const XmlNode *n) {
    const std::string &t = n->tag;
    if (t == "model") { for (auto &c : n->children) storage(c.get()); return; }
    if (t.rfind("store_", 0) == 0) { storage(n); return; }
    if (t == "pos" || t == "hpr" || t == "nhpr" || t == "scale" || t == "color" || t == "title" || t == "texture" ||
        t == "suit_edge" || t == "battle_cell") return;   // fields of the parent
    if (t == "scene" || t == "storage" || t == "#doc") { for (auto &c : n->children) comp(c.get()); return; }
    auto codeColor = [&]() { w.str(n->attr("code")); colorOf(kid(n, "color")); };
    st.components++;
    if (t == "group") { w.u8(C_GROUP); w.str(n->attr("name")); children(n); }
    else if (t == "visgroup") {
      std::string z = n->attr("zone"); w.u8(C_VISGROUP); w.str(z);
      std::vector<std::pair<int, int>> es; for (auto &c : n->children) if (c->tag == "suit_edge") es.push_back({atoi(c->attr("a").c_str()), atoi(c->attr("b").c_str())});
      w.u16((int)es.size()); for (auto &[a, b] : es) { w.u16(a); w.u16(b); edges[a].push_back({b, atoi(z.c_str())}); }
      std::vector<std::string> vis; std::string v = n->attr("vis"); size_t a = 0;
      while (a < v.size()) { size_t e = v.find(' ', a); if (e == std::string::npos) e = v.size(); if (e > a) vis.push_back(v.substr(a, e - a)); a = e + 1; }
      w.u16((int)vis.size()); for (auto &s : vis) w.str(s);
      std::vector<const XmlNode *> cells; for (auto &c : n->children) if (c->tag == "battle_cell") cells.push_back(c.get());
      w.u16((int)cells.size());
      for (auto c : cells) { w.u8(atoi(c->attr("width").c_str())); w.u8(atoi(c->attr("height").c_str())); for (const char *k : {"x", "y", "z"}) w.i32(fx(fl(c->attr(k), 0))); }
      children(n);
    }
    else if (t == "node") { w.u8(C_NODE); w.str(n->attr("name")); node(n); children(n); }
    else if (t == "prop" || t == "anim_prop" || t == "interactive_prop") {
      w.u8(t == "prop" ? C_PROP : t == "anim_prop" ? C_ANIM_PROP : C_INTERACTIVE_PROP); w.str(n->attr("name")); node(n); codeColor();
      if (t != "prop") w.str(n->attr("anim"));
      if (t == "interactive_prop") w.i16(atoi(n->attr("cell_id", "-1").c_str()));
      children(n);
    }
    else if (t == "sign") { w.u8(C_SIGN); w.str(""); node(n); codeColor(); children(n); }
    else if (t == "graphic") {
      w.u8(C_GRAPHIC); w.str(""); node(n); codeColor(); w.i16((int)fx(fl(n->attr("width"), 0))); w.i16((int)fx(fl(n->attr("height"), 0)));
      w.u8(kid(n, "color") ? 0 : 1); children(n);
    }
    else if (t == "baseline") {
      w.u8(C_BASELINE); w.str(""); node(n); codeColor(); w.str(n->attr("flags"));
      for (const char *k : {"indent", "kern", "wiggle", "stumble", "stomp", "width", "height"}) w.f32(fl(n->attr(k), 0));
      children(n);
    }
    else if (t == "text") { w.u8(C_TEXT); w.str(""); node(n); w.str(nodeText(n, src)); codeColor(); children(n); }
    else if (t == "flat_building") {
      bool door = false; for (auto &c : n->children) if (c->tag == "wall" && kid(c.get(), "flat_door")) door = true;
      w.u8(C_FLAT_BUILDING); w.str(n->attr("id", n->attr("name"))); node(n); w.u16((int)trunc6(fl(n->attr("width"), 0) * 10)); w.u8(door);
      children(n);
    }
    else if (t == "wall") { w.u8(C_WALL); w.str(""); node(n); w.str(n->attr("code")); w.i16((int)fx(fl(n->attr("height"), 10))); colorOf(kid(n, "color")); children(n); }
    else if (t == "windows") { w.u8(C_WINDOWS); w.str(""); codeColor(); w.u8(atoi(n->attr("count", "1").c_str())); }
    else if (t == "cornice") { w.u8(C_CORNICE); w.str(""); codeColor(); }
    else if (t == "door" || t == "flat_door") { w.u8(t == "door" ? C_DOOR : C_FLAT_DOOR); w.str(""); codeColor(); }
    else if (t == "landmark_building" || t == "anim_building") {
      std::string id = n->attr("id", n->attr("name"));
      const XmlNode *ti = kid(n, "title"); std::string title = ti ? nodeText(ti, src) : "";
      w.u8(t == "anim_building" ? C_ANIM_BUILDING : C_LANDMARK); w.str(id); node(n); w.str(n->attr("code")); colorOf(kid(n, "color"));
      w.str(title); w.str(n->attr("article")); w.str(n->attr("building_type"));
      if (t == "anim_building") w.str(n->attr("anim"));
      int b = blockOf(id);
      if (b >= 0) { auto &bl = blocks[b]; bl.zone = atoi(zoneOf(n).c_str()); if (ti) bl.title = title;
        if (n->attrs.count("article")) bl.article = n->attr("article"); if (n->attrs.count("building_type")) bl.type = n->attr("building_type"); }
      children(n);
    }
    else if (t == "street") {
      w.u8(C_STREET); w.str(n->attr("name")); node(n); w.str(n->attr("code"));
      for (int i = 0; i < 3; i++) { auto x = kid(n, "texture", i); w.str(x ? nodeText(x, src) : ""); }
      for (int i = 0; i < 3; i++) colorOf(kid(n, "color", i));
    }
    else { st.components--; st.skipped++; skipped.insert(t); }   // not representable (e.g. editor-/game-specific tags)
  }
};
}  // namespace

bool compilePdna(const XmlNode *root, const std::string &src, std::string *bytes, std::string *err, bool compress, CompileStats *stats) {
  Comp c(src);
  W body; std::swap(body, c.w);
  c.w.u8(C_GROUP); c.w.str("root");
  c.comp(root);
  c.w.u8(C_RETURN);
  std::swap(body, c.w);
  W &s = c.w;
  s.u16((int)c.catalog.keys.size());
  for (auto &k : c.catalog.keys) {
    auto &v = c.catalog.m[k];
    if (v.size() > 255) { if (err) *err = "catalog root '" + k + "' has more than 255 codes (format limit)"; return false; }
    s.str(k); s.u8((int)v.size()); for (auto &x : v) s.str(x);
  }
  s.u16((int)c.textures.keys.size()); for (auto &k : c.textures.keys) { s.str(k); s.str(c.textures.m[k]); }
  s.u16((int)c.fonts.keys.size()); for (auto &k : c.fonts.keys) { s.str(k); s.str(c.fonts.m[k]); }
  for (auto &nm : c.nodes) { s.u16((int)nm.keys.size()); for (auto &k : nm.keys) { s.str(k); s.str(nm.m[k].first); s.str(nm.m[k].second); } }
  s.u16((int)c.blocks.keys.size());
  for (int k : c.blocks.keys) { auto &b = c.blocks.m[k]; s.u8(k); s.u16(b.zone); s.str(b.title); s.str(b.article); s.str(b.type); }
  s.u16((int)c.points.size());
  for (auto &p : c.points) { s.u16(p.idx); s.u8(p.type); s.i32(fx(p.x)); s.i32(fx(p.y)); s.i32(fx(p.z)); s.i16(p.lb); }
  s.u16((int)c.edges.keys.size());
  for (int k : c.edges.keys) { auto &v = c.edges.m[k]; s.u16(k); s.u16((int)v.size()); for (auto &[e, z] : v) { s.u16(e); s.u16(z); } }
  std::string data = s.b + body.b;
  if (compress) {
    if (!g_deflate) { if (err) *err = "compressed .pdna needs zlib (not available in this build)"; return false; }
    std::string z; if (!g_deflate(data, &z)) { if (err) *err = "zlib compression failed"; return false; }
    data.swap(z);
  }
  *bytes = std::string("PDNA\n") + (compress ? '\x01' : '\x00') + "\n" + data;
  if (stats) { *stats = c.st; for (auto &t : c.skipped) stats->skippedTags += (stats->skippedTags.empty() ? "" : ", ") + t; }
  return true;
}

}  // namespace dna
