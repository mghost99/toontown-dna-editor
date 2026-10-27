#include "dna_document.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <map>
#include <regex>

namespace fs = std::filesystem;
namespace dna {

const std::set<std::string> kEditable = {"prop", "anim_prop", "interactive_prop", "treasure_spawn",
                                         "flat_building", "landmark_building", "anim_building"};

static bool close(double a, double b) { return std::fabs(a - b) < 1e-4; }
bool Xform::operator==(const Xform &o) const {
  return close(x, o.x) && close(y, o.y) && close(z, o.z) && close(h, o.h) && close(p, o.p) && close(r, o.r) &&
         close(sx, o.sx) && close(sy, o.sy) && close(sz, o.sz);
}
static double fl(const std::string &s, double d) {
  if (s.empty()) return d;
  char *e = nullptr; double v = strtod(s.c_str(), &e); return e == s.c_str() ? d : v;
}
std::string fmtNum(double v) {
  char b[64]; snprintf(b, sizeof b, "%.2f", v); std::string s = b; return s == "-0.00" ? "0.00" : s;
}

static Xform xformOf(const XmlNode *n) {
  Xform x;
  if (auto p = n->child("pos")) { x.x = fl(p->attr("x"), 0); x.y = fl(p->attr("y"), 0); x.z = fl(p->attr("z"), 0); }
  const XmlNode *h = n->child("nhpr"); if (!h) h = n->child("hpr");
  if (h) { x.h = fl(h->attr("h"), 0); x.p = fl(h->attr("p"), 0); x.r = fl(h->attr("r"), 0); }
  if (auto s = n->child("scale")) { x.sx = fl(s->attr("x"), 1); x.sy = fl(s->attr("y"), 1); x.sz = fl(s->attr("z"), 1); }
  return x;
}

bool Document::load(const std::string &absPath, const std::string &res, std::string *err) {
  std::string t; Format f; bool z = false, enc = false;
  if (!loadAny(absPath, &f, &t, err, &z, &enc, &loadError)) { if (err && err->find(absPath) == std::string::npos) *err = absPath + ": " + *err; return false; }
  path = absPath; resRoot = res; text = std::move(t); fmt = f; compressed = z; encrypted = enc;
  rel = fs::relative(absPath, res).generic_string();
  root = fmt == Format::Xml ? parseXml(text) : parseDnaText(text); items.clear(); visgroups.clear();
  int id = 0;
  walk(root.get(), [&](const XmlNode *n) {
    if (n->tag == "visgroup") visgroups.push_back(n->attr("zone"));
    if (!kEditable.count(n->tag)) return;
    Item it; it.origId = id++; it.kind = n->tag; it.node = n;
    it.name = n->attr("name", n->attr("id")); it.code = n->attr("code", n->attr("type"));
    it.width = fl(n->attr("width"), 0);
    for (auto p = n->parent; p; p = p->parent) {
      if (kEditable.count(p->tag)) it.nested = true;
      if (p->tag == "visgroup" && it.visgroup.empty()) it.visgroup = p->attr("zone");
    }
    it.xf = it.origXf = xformOf(n);
    items.push_back(std::move(it));
  });
  return true;
}

int Document::indexOfNode(const XmlNode *n) const {
  for (size_t i = 0; i < items.size(); i++) if (items[i].node == n) return (int)i;
  return -1;
}
bool Document::replaceSource(size_t start, size_t end, const std::string &replacement) {
  if (start > end || end > text.size()) return false;
  text.replace(start, end - start, replacement);
  auto old = std::move(items);
  root = fmt == Format::Xml ? parseXml(text) : parseDnaText(text);
  items.clear(); visgroups.clear();
  size_t original = 0;
  walk(root.get(), [&](const XmlNode *n) {
    if (n->tag == "visgroup") visgroups.push_back(n->attr("zone"));
    if (!kEditable.count(n->tag)) return;
    while (original < old.size() && old[original].origId < 0) ++original;
    Item it;
    if (original < old.size()) it = old[original++];
    it.node = n;
    it.kind = n->tag; it.name = n->attr("name", n->attr("id"));
    it.code = n->attr("code", n->attr("type")); it.width = fl(n->attr("width"), 0);
    it.visgroup.clear(); it.nested = false;
    for (auto p = n->parent; p; p = p->parent) {
      if (kEditable.count(p->tag)) it.nested = true;
      if (p->tag == "visgroup" && it.visgroup.empty()) it.visgroup = p->attr("zone");
    }
    items.push_back(std::move(it));
  });
  // Unsaved additions have no source span. Keep their templates, transforms,
  // indices, and deletion state so existing undo commands remain valid.
  for (auto &it : old) if (it.origId < 0) items.push_back(std::move(it));
  textDirty = true;
  return true;
}

bool Document::setNodeAttribute(const XmlNode *node, const std::string &key, const std::string &value) {
  if (!node || key.empty() || node->start >= node->end) return false;
  std::string xml = toXmlText(node, text, 0);
  auto tree = parseXml(xml);
  if (tree->children.empty()) return false;
  auto target = tree->children.front().get();
  target->attrs[key] = value;
  std::string replacement = fmt == Format::Xml ? toXmlText(target, xml, 0) : toDnaText(target, xml, 0);
  return replaceSource(node->start, node->end, replacement);
}

bool Document::setSignLine(const XmlNode *baseline, const std::string &letters) {
  if (!baseline || baseline->tag != "baseline") return false;
  std::vector<const XmlNode *> texts;
  for (auto &c : baseline->children) if (c->tag == "text") texts.push_back(c.get());
  // Baked geometry contains no editable lettering. Keep it until replacement
  // text is supplied explicitly, then replace only that geometry.
  std::vector<std::string> glyphs;
  for (size_t i = 0; i < letters.size();) {
    size_t end = i + 1;
    while (end < letters.size() && ((unsigned char)letters[end] & 0xc0) == 0x80) ++end;
    glyphs.push_back(letters.substr(i, end - i)); i = end;
  }
  auto escape = [](const std::string &s) {
    std::string out;
    for (char c : s) {
      if (c == '&') out += "&amp;"; else if (c == '<') out += "&lt;";
      else if (c == '>') out += "&gt;"; else if (c == '\"') out += "&quot;"; else out += c;
    }
    return out;
  };
  std::string block;
  for (size_t i = 0; i < glyphs.size(); ++i) {
    std::string xml = texts.empty() ? "<text></text>" : toXmlText(texts[std::min(i, texts.size() - 1)], text, 0);
    auto tree = parseXml(xml); auto entry = tree->children.front().get();
    std::string replacement = escape(glyphs[i]);
    xml.replace(entry->textStart, entry->textEnd - entry->textStart, replacement);
    tree = parseXml(xml);
    block += fmt == Format::Xml ? xml + "\n" : toDnaText(tree->children.front().get(), xml, 0);
  }
  // Edit individual text spans: intervening logos and unknown nodes survive.
  std::vector<std::pair<size_t, size_t>> spans;
  for (auto &c : baseline->children) if (c->tag == "text" || c->tag == "text_geom") spans.push_back({c->start, c->end});
  size_t insert = fmt == Format::Xml ? text.rfind("</", baseline->end - 1) : baseline->end - 1;
  if (!spans.empty()) insert = spans.front().first;
  std::string updated = text;
  for (auto it = spans.rbegin(); it != spans.rend(); ++it) updated.erase(it->first, it->second - it->first);
  updated.insert(insert, block);
  return replaceSource(0, text.size(), updated);
}

bool Document::dirty() const {
  if (textDirty) return true;
  for (auto &it : items) {
    if (it.origId < 0 && !it.deleted) return true;
    if (it.origId >= 0 && (it.deleted || it.xf != it.origXf)) return true;
  }
  return false;
}

// ---- span patching ----
struct Edit { size_t s, e; std::string r; };
static std::string patch(std::string t, std::vector<Edit> edits) {
  std::stable_sort(edits.begin(), edits.end(), [](const Edit &a, const Edit &b) { return a.s > b.s; });
  for (auto &e : edits) t = t.substr(0, e.s) + e.r + t.substr(e.e);
  return t;
}
static std::string lineIndent(const std::string &t, size_t pos) {
  size_t ls = t.rfind('\n', pos ? pos - 1 : 0); ls = (ls == std::string::npos || ls >= pos) ? 0 : ls + 1;
  if (pos == 0) ls = 0;
  size_t i = ls; while (i < t.size() && (t[i] == ' ' || t[i] == '\t')) i++;
  return t.substr(ls, i - ls);
}
static std::string setAttrs(std::string tag, const std::vector<std::pair<std::string, std::string>> &vals) {
  for (auto &[k, v] : vals) {
    std::regex re("(\\b" + k + "\\s*=\\s*)(\"[^\"]*\"|'[^']*')");
    std::smatch m;
    if (std::regex_search(tag, m, re)) {
      tag = tag.substr(0, m.position(0)) + m.str(1) + "\"" + v + "\"" + tag.substr(m.position(0) + m.length(0));
    } else {
      size_t gt = tag.rfind('>'); size_t at = gt; if (at > 0 && tag[at - 1] == '/') at--;
      while (at > 0 && std::isspace((unsigned char)tag[at - 1])) at--;
      tag = tag.substr(0, at) + " " + k + "=\"" + v + "\"" + (tag[gt - 1] == '/' ? " />" : ">");
    }
  }
  return tag;
}

// Rewrites pos/nhpr/scale of element n (in text t) from 'orig' to 'now'; appends edits.
static void transformEdits(const std::string &t, const XmlNode *n, const Xform &orig, const Xform &now,
                           const std::string &tab, std::vector<Edit> &edits, bool dnaText = false) {
  struct G { const char *tag; double o[3], v[3], d; } groups[3] = {
      {"pos", {orig.x, orig.y, orig.z}, {now.x, now.y, now.z}, 0},
      {"nhpr", {orig.h, orig.p, orig.r}, {now.h, now.p, now.r}, 0},
      {"scale", {orig.sx, orig.sy, orig.sz}, {now.sx, now.sy, now.sz}, 1}};
  const char *keys[2][3] = {{"x", "y", "z"}, {"h", "p", "r"}};
  for (int gi = 0; gi < 3; gi++) {
    auto &g = groups[gi];
    if (close(g.o[0], g.v[0]) && close(g.o[1], g.v[1]) && close(g.o[2], g.v[2])) continue;
    const char *const *k = keys[gi == 1 ? 1 : 0];
    const XmlNode *c = n->child(g.tag); if (!c && gi == 1) c = n->child("hpr");
    std::vector<std::pair<std::string, std::string>> vals;
    if (c && dnaText) {   // "pos [ x y z ]": rewrite the whole property, unchanged components keep their text
      std::string s = c->tag + " [";
      for (int j = 0; j < 3; j++) s += " " + (close(g.o[j], g.v[j]) && c->attrs.count(k[j]) ? c->attr(k[j]) : fmtNum(g.v[j]));
      edits.push_back({c->start, c->end, s + " ]"});
    } else if (dnaText) {
      const XmlNode *after = nullptr;
      for (auto &ch : n->children) if (ch->tag == "pos" || ch->tag == "nhpr" || ch->tag == "hpr") after = ch.get();
      size_t at = after ? after->end : n->openEnd;
      std::string ind = after ? lineIndent(t, after->start) : lineIndent(t, n->start) + tab;
      std::string s = "\n" + ind + g.tag + " [";
      for (int j = 0; j < 3; j++) s += " " + fmtNum(g.v[j]);
      edits.push_back({at, at, s + " ]"});
    } else if (c) {
      for (int j = 0; j < 3; j++) if (!close(g.o[j], g.v[j])) vals.push_back({k[j], fmtNum(g.v[j])});
      edits.push_back({c->start, c->openEnd, setAttrs(t.substr(c->start, c->openEnd - c->start), vals)});
    } else {
      const XmlNode *after = nullptr;
      for (auto &ch : n->children) if (ch->tag == "pos" || ch->tag == "nhpr" || ch->tag == "hpr") after = ch.get();
      size_t at = after ? after->end : n->openEnd;
      std::string ind = after ? lineIndent(t, after->start) : lineIndent(t, n->start) + tab;
      std::string s = "\n" + ind + "<" + g.tag;
      for (int j = 0; j < 3; j++) s += std::string(" ") + k[j] + "=\"" + fmtNum(g.v[j]) + "\"";
      edits.push_back({at, at, s + " />"});
    }
  }
}

std::string Document::buildSave(SaveStats *st) const {
  SaveStats s; std::vector<Edit> edits; bool dnaText = fmt != Format::Xml;
  std::string tab = text.find("\n\t") != std::string::npos ? "\t" : "    ";
  std::set<const XmlNode *> gone;
  for (auto &it : items) if (it.origId >= 0 && it.deleted) gone.insert(it.node);
  for (auto &it : items) {
    if (it.origId < 0) continue;
    bool underGone = false;
    for (auto p = it.node->parent; p; p = p->parent) if (gone.count(p)) underGone = true;
    if (underGone) continue;
    if (it.deleted) {
      size_t ls = text.rfind('\n', it.node->start ? it.node->start - 1 : 0);
      edits.push_back({ls == std::string::npos ? it.node->start : ls, it.node->end, ""}); s.deleted++;
      continue;
    }
    if (it.xf != it.origXf) { transformEdits(text, it.node, it.origXf, it.xf, tab, edits, dnaText); s.moved++; }
  }
  // additions: into their visgroup (or first visgroup / first element)
  std::map<std::string, const XmlNode *> vg; const XmlNode *firstVg = nullptr;
  walk(root.get(), [&](const XmlNode *n) { if (n->tag == "visgroup") { if (!firstVg) firstVg = n; vg.emplace(n->attr("zone"), n); } });
  std::map<size_t, std::string> ins;
  for (auto &it : items) {
    if (it.origId >= 0 || it.deleted) continue;
    const XmlNode *host = vg.count(it.visgroup) ? vg[it.visgroup] : firstVg;
    if (!host && !root->children.empty()) host = root->children[0].get();
    if (!host) continue;
    size_t endtag = dnaText ? host->end - 1 : text.rfind("</", host->end - 1);
    if (endtag == std::string::npos || endtag < host->start) endtag = host->end;
    std::string ind = lineIndent(text, host->start) + tab;
    // template element, re-indented and with its transform patched
    std::string tpl = it.templateXml;
    auto sub = parseXml(tpl);
    if (!sub->children.empty()) {
      std::vector<Edit> e2; transformEdits(tpl, sub->children[0].get(), Xform{} == it.origXf ? it.origXf : it.origXf, it.xf, tab, e2);
      tpl = patch(tpl, e2);
      if (dnaText) { auto px = parseXml(tpl); tpl = px->children.empty() ? "" : toDnaText(px->children[0].get(), tpl, 0); }
    }
    // normalise indentation: strip common leading whitespace of each line, prefix with ind
    std::string out, line; size_t i = 0; std::string base;
    { auto r = parseXml(tpl); }
    std::vector<std::string> lines; size_t a = 0;
    while (a <= tpl.size()) { size_t b = tpl.find('\n', a); if (b == std::string::npos) b = tpl.size(); lines.push_back(tpl.substr(a, b - a)); a = b + 1; }
    size_t common = std::string::npos;
    for (size_t li = 1; li < lines.size(); li++) {
      auto &l = lines[li]; size_t k = l.find_first_not_of(" \t"); if (k == std::string::npos) continue;
      common = std::min(common, k);
    }
    for (size_t li = 0; li < lines.size(); li++) {
      std::string l = lines[li];
      if (l.find_first_not_of(" \t\r") == std::string::npos) continue;
      if (li == 0) { size_t k = l.find_first_not_of(" \t"); l = l.substr(k); }
      else if (common != std::string::npos) l = l.substr(std::min(common, l.size()));
      out += ind + l + "\n";
    }
    (void)i; (void)line; (void)base;
    size_t ls = text.rfind('\n', endtag ? endtag - 1 : 0); ls = ls == std::string::npos ? 0 : ls + 1;
    ins[ls] += out; s.added++;
  }
  for (auto &[at, blk] : ins) edits.push_back({at, at, blk});
  if (st) *st = s;
  return dnaText ? withDnaHeader(patch(text, edits), rel) : withHeader(patch(text, edits), rel);
}

std::string Document::itemXml(const Item &it) const {
  if (it.origId < 0 || !it.node) return it.templateXml;
  if (fmt == Format::Xml) return text.substr(it.node->start, it.node->end - it.node->start);
  return toXmlText(it.node, text, 0);
}

static Format targetFormat(const std::string &p, Format dflt) {
  std::string e = fs::path(p).extension().string(); for (auto &ch : e) ch = (char)std::tolower((unsigned char)ch);
  return e == ".pdna" ? Format::Pdna : e == ".dna" ? Format::Dna : e == ".xml" ? Format::Xml : dflt;
}

bool Document::save(std::string *err, SaveStats *st, const std::string &outPath) {
  std::string target = outPath.empty() ? path : outPath;
  Format tf = outPath.empty() ? fmt : targetFormat(outPath, fmt);
  std::string data = buildSave(st);
  // sanity: the result must still parse to the same root tag and keep every element balanced
  int perr = 0;
  auto chk = fmt == Format::Xml ? parseXml(data) : parseDnaText(data, &perr);
  if (chk->children.empty() || perr || (root->children.size() && chk->children[0]->tag != root->children[0]->tag)) {
    if (err) *err = std::string("refusing to save: patched ") + formatName(fmt) + " does not parse back"; return false;
  }
  if (tf == Format::Pdna) {
    if (!compilePdna(chk.get(), data, &data, err, tf == fmt ? compressed : false, &lastCompile)) return false;
    if (encrypted && (tf == fmt || fmt != Format::Pdna)) { std::string e; if (!encryptWithSession(data, &e, err)) return false; data.swap(e); }
  } else if (tf == Format::Dna && fmt == Format::Xml) {
    data = withDnaHeader(dnaDocument(chk.get(), data), rel);
  } else if (tf == Format::Xml && fmt != Format::Xml) {
    data = withHeader(xmlDocument(chk.get(), data), rel);
  }
  std::error_code ec;
  if (fs::exists(target)) fs::copy_file(target, target + ".bak", fs::copy_options::overwrite_existing, ec);
  if (ec) { if (err) *err = "backup failed: " + ec.message(); return false; }
  std::string tmp = target + ".tmp";
  if (!writeFile(tmp, data)) { if (err) *err = "cannot write " + tmp; return false; }
  fs::rename(tmp, target, ec);
  if (ec) { if (err) *err = "rename failed: " + ec.message(); return false; }
  textDirty = false;
  return true;
}

std::string withDnaHeader(const std::string &text0, const std::string &rel) {
  const std::string tag = "// DNA Editor: ";
  std::string nl = text0.find("\r\n") != std::string::npos ? "\r\n" : "\n";
  std::string text = text0;
  while (text.compare(0, tag.size(), tag) == 0) { size_t e = text.find('\n'); text.erase(0, e == std::string::npos ? text.size() : e + 1); }
  char tb[64]; time_t now = time(nullptr); strftime(tb, sizeof tb, "%Y-%m-%dT%H:%M:%S%z", localtime(&now));
  return tag + "saved " + tb + " from " + rel + nl + text;
}

std::string withHeader(const std::string &text0, const std::string &rel) {
  const std::string tool = "Toontown DNA Editor";
  std::string nl = text0.find("\r\n") != std::string::npos ? "\r\n" : "\n";
  char tb[64]; time_t now = time(nullptr); strftime(tb, sizeof tb, "%Y-%m-%dT%H:%M:%S%z", localtime(&now));
  std::string block = "<!-- " + tool + nl + "     tool: " + tool + " (native)" + nl + "     version: 3.0" + nl +
                      "     generated: " + tb + nl + "     source: " + rel + nl + "-->" + nl;
  std::string text = text0;
  size_t h = text.find("<!--");
  if (h != std::string::npos && text.compare(h, 5 + tool.size(), "<!-- " + tool) == 0) {
    size_t e = text.find("-->", h); if (e != std::string::npos) {
      e += 3; while (e < text.size() && (text[e] == '\r' || text[e] == '\n')) { e++; if (text[e - 1] == '\n') break; }
      size_t s = h; while (s > 0 && (text[s - 1] == ' ' || text[s - 1] == '\t')) s--;
      text.erase(s, e - s);
    }
  }
  size_t a = 0; if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) a = 3;
  if (text.compare(a, 5, "<?xml") == 0) {
    size_t e = text.find("?>", a); e += 2; while (e < text.size() && (text[e] == ' ' || text[e] == '\t')) e++;
    if (e < text.size() && text[e] == '\r') e++;
    if (e < text.size() && text[e] == '\n') e++; else text.insert(e, nl), e += nl.size();
    return text.substr(0, e) + block + text.substr(e);
  }
  return block + text;
}

// ---- storage selection (port of render_worker.storages_for_file) ----
static const std::vector<std::pair<std::string, std::string>> kHoods = {
    {"donalds_dreamland", "DL"}, {"daisys_garden", "DG"}, {"the_burrrgh", "BR"},
    {"toontown_central", "TT"}, {"minnies_melodyland", "MM"}, {"donalds_dock", "DD"}, {"goofy_speedway", "GS"},
    {"outdoor_zone", "OZ"}, {"golf_zone", "GZ"}, {"cog_hq", "CC"}, {"party", "party"}, {"estate", "estate"},
    {"tutorial", "tutorial"}};

static std::map<std::string, std::string> &dnaIndex(const std::string &res) {   // basename -> rel
  static std::map<std::string, std::map<std::string, std::string>> cache;
  auto &m = cache[res];
  if (m.empty()) {
    for (auto &ph : fs::directory_iterator(res)) {
      if (!ph.is_directory() || ph.path().filename().string().rfind("phase_", 0) != 0) continue;
      fs::path d = ph.path() / "dna"; if (!fs::is_directory(d)) continue;
      for (auto &f : fs::directory_iterator(d))
        if (f.path().extension() == ".xml" || f.path().extension() == ".dna" || f.path().extension() == ".pdna") {
          auto b = f.path().filename().string();
          auto r = fs::relative(f.path(), res).generic_string();
          if (!m.count(b) || r < m[b]) m[b] = r;
        }
    }
  }
  return m;
}
static std::set<std::string> &storageCodes(const std::string &res, const std::string &rel) {
  static std::map<std::string, std::set<std::string>> cache;
  auto it = cache.find(rel); if (it != cache.end()) return it->second;
  auto &s = cache[rel];
  auto x = parseFile(res + "/" + rel);
  walk(x.get(), [&](const XmlNode *n) { if (n->tag.rfind("store_", 0) == 0 && n->tag != "store_suit_point") s.insert(n->attr("code")); });
  return s;
}

std::vector<std::string> storagesForFile(const std::string &res, const std::string &rel, const std::string &text,
                                         std::vector<std::string> *undefinedOut) {
  auto &idx = dnaIndex(res);
  std::string b = fs::path(rel).filename().string(), code;
  for (auto &[k, v] : kHoods) if (b.rfind(k, 0) == 0) { code = v; break; }
  bool digits = std::any_of(b.begin(), b.end(), ::isdigit);
  bool sz = b.find("_sz") != std::string::npos || !digits;
  std::vector<std::string> names = {"storage.xml"}; if (!sz) names.push_back("storage_town.xml");
  std::vector<std::string> hoods; if (code == "PP") hoods = {"DL", "PP"}; else if (!code.empty()) hoods = {code};
  for (auto &h : hoods) { names.push_back("storage_" + h + ".xml"); names.push_back("storage_" + h + (sz ? "_sz.xml" : "_town.xml")); }
  std::vector<std::string> pref;
  for (auto &n : names) {   // same stem in any format; XML first, then .dna, then .pdna
    std::string stem = n.substr(0, n.size() - 4);
    for (auto ext : {".xml", ".dna", ".pdna"}) if (idx.count(stem + ext)) { pref.push_back(idx[stem + ext]); break; }
  }
  std::set<std::string> have; for (auto &p : pref) { auto &c = storageCodes(res, p); have.insert(c.begin(), c.end()); }
  std::set<std::string> used;
  { auto x = parseText(text); walk(x.get(), [&](const XmlNode *n) { auto it = n->attrs.find("code"); if (it != n->attrs.end() && !it->second.empty() && !have.count(it->second)) used.insert(it->second); }); }
  std::vector<std::string> extra;
  for (auto &[bn, r] : idx) {
    if (bn.rfind("storage", 0) != 0 || used.empty()) continue;
    if (std::find(pref.begin(), pref.end(), r) != pref.end()) continue;
    auto &c = storageCodes(res, r); bool hit = false;
    for (auto it = used.begin(); it != used.end();) { if (c.count(*it)) { hit = true; it = used.erase(it); } else ++it; }
    if (hit) extra.push_back(r);
  }
  if (undefinedOut) undefinedOut->assign(used.begin(), used.end());
  extra.insert(extra.end(), pref.begin(), pref.end());   // hood files last so their codes win
  return extra;
}

std::vector<std::string> sceneFiles(const std::string &res) {
  std::vector<std::string> out;
  for (auto &[b, r] : dnaIndex(res)) {
    if (b.find("storage") != std::string::npos) continue;
    if (fs::path(r).extension() == ".xml") { std::string head = readFile(res + "/" + r).substr(0, 400); if (head.find("<storage") != std::string::npos) continue; }
    out.push_back(r);
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::vector<CatalogEntry> loadCatalog(const std::string &res) {
  std::map<std::string, CatalogEntry> seen;
  for (auto &[b, r] : dnaIndex(res)) {
    bool isXml = fs::path(r).extension() == ".xml";
    if (isXml) { std::string t = readFile(res + "/" + r); if (t.substr(0, 400).find("<storage") == std::string::npos) continue; }
    else if (b.find("storage") == std::string::npos) continue;
    auto x = parseFile(res + "/" + r);
    walk(x.get(), [&](const XmlNode *n) {
      if (n->tag != "store_node") return;
      std::string root = n->attr("root");
      if (root != "prop" && root != "holiday_prop" && root != "anim_prop" && root != "interactive_prop") return;
      std::string c = n->attr("code"); if (c.empty() || seen.count(c)) return;
      seen[c] = {c, root, n->parent ? n->parent->attr("path") : "", n->attrs.count("node") ? n->attr("node") : c, r};
    });
  }
  std::vector<CatalogEntry> out; for (auto &[k, v] : seen) out.push_back(v);
  auto pri = [](const std::string &c) { for (auto p : {"st_", "pp_", "prop_pp", "cog"}) if (c.rfind(p, 0) == 0) return 0; return 1; };
  std::sort(out.begin(), out.end(), [&](auto &a, auto &b) { return std::make_pair(pri(a.code), a.code) < std::make_pair(pri(b.code), b.code); });
  return out;
}

}  // namespace dna
