// Text DNA (.dna) <-> common tree, XML/DNA serialisers, format detection and loading.
#include "dna_formats.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <set>

namespace fs = std::filesystem;
namespace dna {

const char *formatName(Format f) { return f == Format::Xml ? "XML" : f == Format::Dna ? "DNA" : "PDNA"; }

Format textFormat(const std::string &t) {
  size_t i = t.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0;
  while (i < t.size() && std::isspace((unsigned char)t[i])) i++;
  return i < t.size() && t[i] == '<' ? Format::Xml : Format::Dna;
}

Format detectFormat(const std::string &path, const std::string &bytes) {
  std::string ext = fs::path(path).extension().string();
  for (auto &c : ext) c = (char)std::tolower((unsigned char)c);
  if (ext == ".pdna") return Format::Pdna;
  if (ext == ".dna") return Format::Dna;
  if (ext == ".xml") return Format::Xml;
  if (bytes.compare(0, 5, "PDNA\n") == 0) return Format::Pdna;
  return textFormat(bytes);
}

// ---------------------------------------------------------------- tokenizer
namespace {
enum TT { LB, RB, COMMA, STR, WORD, END };
struct Tok { TT t; size_t s, e; };   // [s,e) in source; STR includes the quotes

std::vector<Tok> tokenize(const std::string &x) {
  std::vector<Tok> v; size_t i = 0, n = x.size();
  while (i < n) {
    char c = x[i];
    if (std::isspace((unsigned char)c)) { i++; continue; }
    if (c == '/' && i + 1 < n && x[i + 1] == '/') { while (i < n && x[i] != '\n') i++; continue; }
    if (c == '/' && i + 1 < n && x[i + 1] == '*') { size_t e = x.find("*/", i + 2); i = e == std::string::npos ? n : e + 2; continue; }
    if (c == '[') { v.push_back({LB, i, i + 1}); i++; continue; }
    if (c == ']') { v.push_back({RB, i, i + 1}); i++; continue; }
    if (c == ',') { v.push_back({COMMA, i, i + 1}); i++; continue; }
    if (c == '"') {
      size_t e = i + 1;
      while (e < n) { if (x[e] == '\\' && e + 1 < n) { e += 2; continue; } if (x[e++] == '"') break; }
      v.push_back({STR, i, e}); i = e; continue;
    }
    size_t s = i;
    while (i < n && !std::isspace((unsigned char)x[i]) && x[i] != '[' && x[i] != ']' && x[i] != ',' && x[i] != '"') i++;
    v.push_back({WORD, s, i});
  }
  v.push_back({END, n, n});
  return v;
}

struct Raw {
  std::string tag; std::vector<Tok> head, vals; std::vector<std::unique_ptr<Raw>> kids;
  size_t start = 0, openEnd = 0, end = 0;
};

struct P {
  const std::string &x; std::vector<Tok> t; size_t i = 0; int errors = 0;
  explicit P(const std::string &s) : x(s), t(tokenize(s)) {}
  bool elementAt(size_t j) const {
    if (t[j].t != WORD) return false;
    j++; while (t[j].t == STR) j++;
    return t[j].t == LB;
  }
  std::unique_ptr<Raw> element() {
    auto r = std::make_unique<Raw>();
    r->start = t[i].s; r->tag = x.substr(t[i].s, t[i].e - t[i].s); i++;
    while (t[i].t == STR) r->head.push_back(t[i++]);
    r->openEnd = t[i].e; i++;   // '['
    body(*r);
    if (t[i].t == RB) { r->end = t[i].e; i++; } else { errors++; r->end = t[i].s; }
    return r;
  }
  void body(Raw &r) {
    while (t[i].t != END && t[i].t != RB) {
      if (elementAt(i)) r.kids.push_back(element());
      else if (t[i].t == LB) { errors++; i++; }
      else { if (t[i].t != COMMA) r.vals.push_back(t[i]); i++; }
    }
  }
};

const std::set<std::string> kNamed = {"group", "visgroup", "node", "prop", "anim_prop", "interactive_prop",
                                      "flat_building", "landmark_building", "anim_building", "street"};
const std::set<std::string> kComponents = {"group", "visgroup", "node", "prop", "anim_prop", "interactive_prop", "sign",
                                           "baseline", "text", "graphic", "flat_building", "wall", "windows", "cornice",
                                           "landmark_building", "anim_building", "door", "flat_door", "street"};
const std::set<std::string> kNumeric = {"width", "height", "count", "cell_id", "kern", "wiggle", "stumble", "stomp", "indent"};

std::string headAttr(const std::string &tag) {
  if (tag == "visgroup") return "zone";
  if (tag == "flat_building" || tag == "landmark_building" || tag == "anim_building") return "id";
  return "name";
}
}  // namespace

static std::string tokVal(const std::string &x, const Tok &k) {
  if (k.t == STR) {
    std::string out;
    for (size_t i = k.s + 1; i + 1 < k.e; ++i) {
      if (x[i] == '\\' && i + 2 < k.e && (x[i + 1] == '\\' || x[i + 1] == '"')) ++i;
      out += x[i];
    }
    return out;
  }
  return x.substr(k.s, k.e - k.s);
}

static void convert(const std::string &x, Raw &r, XmlNode *parent) {
  const std::string &t = r.tag;
  auto vals = [&](size_t k) { return k < r.vals.size() ? tokVal(x, r.vals[k]) : std::string(); };
  auto child = [&](const std::string &tag) {
    auto n = std::make_unique<XmlNode>(); n->tag = tag; n->start = r.start; n->openEnd = r.openEnd; n->end = r.end;
    n->textStart = n->textEnd = r.openEnd; n->parent = parent; XmlNode *p = n.get(); parent->children.push_back(std::move(n)); return p;
  };
  bool leaf = r.kids.empty() && r.head.empty();
  if (leaf && (t == "pos" || t == "scale" || t == "hpr" || t == "nhpr")) {
    auto n = child(t); const char *k = (t == "pos" || t == "scale") ? "xyz" : "hpr";
    for (int j = 0; j < 3; j++) n->attrs[std::string(1, k[j])] = vals(j).empty() ? (t == "scale" ? (j ? n->attrs["x"] : "1") : "0") : vals(j);
    return;
  }
  if (leaf && t == "color") { auto n = child(t); n->attrs = {{"r", vals(0)}, {"g", vals(1)}, {"b", vals(2)}, {"a", r.vals.size() > 3 ? vals(3) : "1"}}; return; }
  if (leaf && (t == "title" || t == "texture")) {
    auto n = child(t); if (!r.vals.empty()) { n->textStart = r.vals[0].s + (r.vals[0].t == STR); n->textEnd = r.vals[0].e - (r.vals[0].t == STR); }
    return;
  }
  if (leaf && t == "letters") {
    if (!r.vals.empty()) { parent->textStart = r.vals[0].s + (r.vals[0].t == STR); parent->textEnd = r.vals[0].e - (r.vals[0].t == STR); }
    return;
  }
  if (leaf && t == "vis") { std::string s; for (size_t j = 0; j < r.vals.size(); j++) s += (j ? " " : "") + vals(j); parent->attrs["vis"] = s; return; }
  if (leaf && t == "suit_edge") { auto n = child(t); n->attrs = {{"a", vals(0)}, {"b", vals(1)}}; return; }
  if (leaf && t == "battle_cell") { auto n = child(t); n->attrs = {{"width", vals(0)}, {"height", vals(1)}, {"x", vals(2)}, {"y", vals(3)}, {"z", vals(4)}}; return; }
  if (leaf && t == "store_suit_point") {
    auto n = child(t); n->attrs = {{"id", vals(0)}, {"type", vals(1)}, {"x", vals(2)}, {"y", vals(3)}, {"z", vals(4)}};
    if (r.vals.size() > 5) n->attrs["building"] = vals(5);
    return;
  }
  if (leaf && t == "store_node") { auto n = child(t); n->attrs = {{"root", vals(0)}, {"code", vals(1)}}; if (r.vals.size() > 2) n->attrs["node"] = vals(2); return; }
  if (leaf && t == "store_texture") {
    auto n = child(t);
    if (r.vals.size() > 2) n->attrs = {{"root", vals(0)}, {"code", vals(1)}, {"path", vals(2)}}; else n->attrs = {{"code", vals(0)}, {"path", vals(1)}};
    return;
  }
  if (leaf && t == "store_font") { auto n = child(t); n->attrs = {{"root", vals(0)}, {"code", vals(1)}, {"path", vals(2)}}; return; }
  if (t == "model" || t == "hood_model" || t == "place_model") {
    auto n = child("model"); n->attrs["path"] = r.head.empty() ? vals(0) : tokVal(x, r.head[0]);
    n->attrs["scope"] = t == "hood_model" ? "hood" : t == "place_model" ? "place" : "global";
    for (auto &k : r.kids) convert(x, *k, n);
    return;
  }
  if (leaf && !kComponents.count(t) && r.vals.size() == 1 && parent->tag != "#doc") { parent->attrs[t] = vals(0); return; }
  // component (or unknown element): keep as a node; unknown leaf values are kept as "values"
  auto n = child(t);
  if (!r.head.empty()) n->attrs[headAttr(t)] = tokVal(x, r.head[0]);
  else if (kNamed.count(t)) n->attrs[headAttr(t)] = "";
  if (!kComponents.count(t) && !r.vals.empty()) { std::string s; for (size_t j = 0; j < r.vals.size(); j++) s += (j ? " " : "") + vals(j); n->attrs["values"] = s; }
  for (auto &k : r.kids) convert(x, *k, n);
}

std::unique_ptr<XmlNode> parseDnaText(const std::string &text, int *errors) {
  auto root = std::make_unique<XmlNode>(); root->tag = "#doc"; root->end = text.size();
  P p(text); Raw top; p.body(top);
  while (p.t[p.i].t != END) { p.errors++; p.i++; p.body(top); }   // stray ']'
  for (auto &k : top.kids) convert(text, *k, root.get());
  if (errors) *errors = p.errors + (int)top.vals.size();
  return root;
}

std::unique_ptr<XmlNode> parseText(const std::string &text) {
  return textFormat(text) == Format::Xml ? parseXml(text) : parseDnaText(text);
}

static std::string unesc(std::string s) {
  static const std::pair<const char *, const char *> m[] = {{"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"}, {"&amp;", "&"}};
  for (auto &[a, b] : m) for (size_t p = 0; (p = s.find(a, p)) != std::string::npos; p += strlen(b)) s.replace(p, strlen(a), b);
  return s;
}
static std::string esc(const std::string &s) {
  std::string o; for (char c : s) { if (c == '&') o += "&amp;"; else if (c == '<') o += "&lt;"; else if (c == '>') o += "&gt;"; else if (c == '"') o += "&quot;"; else o += c; }
  return o;
}

std::string nodeText(const XmlNode *n, const std::string &src) {
  std::string value = n->tag == "text" && n->textEnd > n->textStart
    ? src.substr(n->textStart, n->textEnd - n->textStart) : n->text(src);
  if (value.find_first_of("\r\n") != std::string::npos) value = n->text(src);
  if (textFormat(src) == Format::Dna && n->tag == "text") {
    std::string decoded;
    for (size_t i = 0; i < value.size(); ++i) {
      if (value[i] == '\\' && i + 1 < value.size() && (value[i + 1] == '\\' || value[i + 1] == '"')) ++i;
      decoded += value[i];
    }
    return decoded;
  }
  return unesc(value);
}

// ---------------------------------------------------------------- DNA writer
static std::string q(const std::string &s) {
  std::string out = "\"";
  for (char c : s) { if (c == '\\' || c == '"') out += '\\'; out += c; }
  return out + "\"";
}
static std::string A(const XmlNode *n, const char *k, const char *d = "") { return unesc(n->attr(k, d)); }

std::string toDnaText(const XmlNode *n, const std::string &src, int depth) {
  const std::string &t = n->tag; std::string ind(depth, '\t'), out;
  if (t == "#doc" || t == "scene" || t == "storage") {
    for (auto &c : n->children) out += toDnaText(c.get(), src, depth);
    return out;
  }
  if (t == "pos" || t == "scale") return ind + t + " [ " + A(n, "x", "0") + " " + A(n, "y", "0") + " " + A(n, "z", "0") + " ]\n";
  if (t == "hpr" || t == "nhpr") return ind + t + " [ " + A(n, "h", "0") + " " + A(n, "p", "0") + " " + A(n, "r", "0") + " ]\n";
  if (t == "color") return ind + "color [ " + A(n, "r", "1") + " " + A(n, "g", "1") + " " + A(n, "b", "1") + " " + A(n, "a", "1") + " ]\n";
  if (t == "title" || t == "texture") return ind + t + " [ " + q(nodeText(n, src)) + " ]\n";
  if (t == "suit_edge") return ind + "suit_edge [ " + A(n, "a") + " " + A(n, "b") + " ]\n";
  if (t == "battle_cell") return ind + "battle_cell [ " + A(n, "width") + " " + A(n, "height") + " " + A(n, "x", "0") + " " + A(n, "y", "0") + " " + A(n, "z", "0") + " ]\n";
  if (t == "store_suit_point") {
    std::string s = ind + "store_suit_point [ " + A(n, "id") + ", " + A(n, "type", "STREET_POINT") + ", " + A(n, "x", "0") + " " + A(n, "y", "0") + " " + A(n, "z", "0");
    if (n->attrs.count("building")) s += ", " + A(n, "building");
    return s + " ]\n";
  }
  if (t == "store_node") return ind + "store_node [ " + q(A(n, "root")) + " " + q(A(n, "code")) + (n->attrs.count("node") ? " " + q(A(n, "node")) : "") + " ]\n";
  if (t == "store_texture") return ind + "store_texture [ " + (n->attrs.count("root") ? q(A(n, "root")) + " " : "") + q(A(n, "code")) + " " + q(A(n, "path")) + " ]\n";
  if (t == "store_font") return ind + "store_font [ " + q(A(n, "root")) + " " + q(A(n, "code")) + " " + q(A(n, "path")) + " ]\n";
  if (t == "model") {
    std::string s = n->attr("scope"); std::string kw = s == "hood" ? "hood_model" : s == "place" ? "place_model" : "model";
    out = ind + kw + " " + q(A(n, "path")) + " [\n";
    for (auto &c : n->children) out += toDnaText(c.get(), src, depth + 1);
    return out + ind + "]\n";
  }
  std::string ha = headAttr(t);
  out = ind + t;
  if (kNamed.count(t) || (!kComponents.count(t) && n->attrs.count(ha))) out += " " + q(A(n, ha.c_str()));
  out += " [\n";
  std::string in2(depth + 1, '\t');
  if (n->attrs.count("vis")) {
    out += in2 + "vis [";
    std::string v = A(n, "vis"); size_t a = 0;
    while (a < v.size()) { size_t b = v.find(' ', a); if (b == std::string::npos) b = v.size(); if (b > a) out += " " + q(v.substr(a, b - a)); a = b + 1; }
    out += " ]\n";
  }
  if (t == "text") out += in2 + "letters [ " + q(nodeText(n, src)) + " ]\n";
  auto emit = [&](const std::string &k, const std::string &v) {
    out += in2 + k + " [ " + (kNumeric.count(k) ? v : q(v)) + " ]\n";
  };
  if (n->attrs.count("code")) emit("code", A(n, "code"));
  for (auto &[k, v] : n->attrs) {
    if (k == "code" || k == "vis" || (k == ha && (kNamed.count(t) || !kComponents.count(t)))) continue;
    if (k == "values") { out += in2 + "// values: " + v + "\n"; continue; }
    emit(k, unesc(v));
  }
  for (auto &c : n->children) out += toDnaText(c.get(), src, depth + 1);
  return out + ind + "]\n";
}

std::string dnaDocument(const XmlNode *root, const std::string &src) { return toDnaText(root, src, 0); }

// ---------------------------------------------------------------- XML writer
std::string toXmlText(const XmlNode *n, const std::string &src, int depth) {
  if (n->tag == "#doc") { std::string o; for (auto &c : n->children) o += toXmlText(c.get(), src, depth); return o; }
  std::string ind(depth, '\t'), o = ind + "<" + n->tag;
  for (auto &[k, v] : n->attrs) o += " " + k + "=\"" + esc(unesc(v)) + "\"";
  std::string txt = (n->textEnd > n->textStart) ? nodeText(n, src) : "";
  if (n->children.empty() && txt.empty()) return o + " />\n";
  if (n->children.empty()) return o + ">" + esc(txt) + "</" + n->tag + ">\n";
  if (n->tag == "text") {
    o += ">" + esc(txt);
    for (auto &c : n->children) { auto child = toXmlText(c.get(), src, 0); o += child; }
    return o + "</text>\n";
  }
  o += ">" + esc(txt) + "\n";
  for (auto &c : n->children) o += toXmlText(c.get(), src, depth + 1);
  return o + ind + "</" + n->tag + ">\n";
}

std::string xmlDocument(const XmlNode *root, const std::string &src) {
  std::string o = "<?xml version='1.0' encoding='utf-8'?>\n";
  bool wrapped = root->children.size() == 1 && (root->children[0]->tag == "scene" || root->children[0]->tag == "storage");
  if (wrapped) return o + toXmlText(root, src, 0);
  bool storage = !root->children.empty();
  for (auto &c : root->children) if (c->tag != "model" && c->tag.rfind("store_", 0) != 0) storage = false;
  for (auto &c : root->children) if (c->tag == "store_suit_point") storage = false;
  std::string w = storage ? "storage" : "scene";
  o += "<" + w + ">\n";
  for (auto &c : root->children) o += toXmlText(c.get(), src, 1);
  return o + "</" + w + ">\n";
}

// ---------------------------------------------------------------- loading
static CryptFn g_decrypt, g_encrypt;
static std::string g_password;   // session only
void setCipher(CryptFn d, CryptFn e) { g_decrypt = std::move(d); g_encrypt = std::move(e); }
void setSessionPassword(const std::string &pw) { g_password = pw; }
bool haveSessionPassword() { return !g_password.empty(); }

static bool looksText(const std::string &b) {
  size_t n = std::min<size_t>(b.size(), 512), bin = 0;
  for (size_t i = 0; i < n; i++) { unsigned char c = b[i]; if (c < 9 || (c > 13 && c < 32)) bin++; }
  return n && bin == 0;
}

bool encryptWithSession(const std::string &plain, std::string *out, std::string *err) {
  if (!g_encrypt || g_password.empty()) { if (err) *err = "no cipher/password for an encrypted .pdna"; return false; }
  if (!g_encrypt(plain, g_password, out)) { if (err) *err = "encryption failed"; return false; }
  return true;
}

bool loadAny(const std::string &path, Format *fmt, std::string *text, std::string *err, bool *compressed, bool *encrypted, LoadError *why) {
  if (why) *why = LoadError::None;
  if (encrypted) *encrypted = false;
  bool ok; std::string b = readFile(path, &ok);
  if (!ok) { if (err) *err = "cannot read " + path; if (why) *why = LoadError::Io; return false; }
  Format f = detectFormat(path, b);
  if (f == Format::Pdna && b.compare(0, 5, "PDNA\n") != 0) {
    if (looksText(b)) f = textFormat(b);   // .pdna name but text inside
    else {
      if (!g_decrypt) { if (err) *err = "file looks encrypted but this build has no decryption support"; if (why) *why = LoadError::Format; return false; }
      if (g_password.empty()) { if (err) *err = "this .pdna is encrypted: a password is needed"; if (why) *why = LoadError::NeedPassword; return false; }
      std::string plain;
      if (!g_decrypt(b, g_password, &plain) || plain.compare(0, 5, "PDNA\n") != 0) {
        if (err) *err = "wrong password (or an unsupported encryption scheme)"; if (why) *why = LoadError::WrongPassword; return false;
      }
      b.swap(plain); if (encrypted) *encrypted = true;
    }
  }
  if (fmt) *fmt = f;
  if (f == Format::Pdna) { bool r = decompilePdna(b, text, err, compressed); if (!r && why) *why = LoadError::Format; return r; }
  if (compressed) *compressed = false;
  if (text) *text = std::move(b);
  return true;
}

std::unique_ptr<XmlNode> parseFile(const std::string &path, std::string *textOut, Format *fmtOut) {
  Format f; std::string t, e;
  if (!loadAny(path, &f, &t, &e)) { auto r = std::make_unique<XmlNode>(); r->tag = "#doc"; if (textOut) textOut->clear(); return r; }
  auto r = f == Format::Xml ? parseXml(t) : parseDnaText(t);
  if (fmtOut) *fmtOut = f;
  if (textOut) *textOut = std::move(t);
  return r;
}

}  // namespace dna
