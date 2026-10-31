#pragma once
// Sign font resolution shared by every DNA format (.xml, .dna and .pdna all become the same
// node tree, and scene.cpp's signFont() calls resolveSignFont() for every baseline).
//
// Fonts are found by SHORT NAME in the phase font folders (phase_*/fonts, phase_*/models/fonts)
// of every model-path root, indexed once at first use. Lookup order for a requested name
// (a storage path, a code such as "mickey", or a bare name such as "SignFont"):
//   1. alias table (codes and game globals -> the short name the game uses, see TTLocalizerEnglish)
//   2. exact short name (case-insensitive, path/extension/separators ignored)
//   3. same name with common suffixes stripped (Font/Classic/Standard/Maximum/Regular/Bold),
//      preferring the variant the game uses by default
//   4. prefix / contains match
//   5. edit distance
//   6. the game's default sign font ("no match -> default")
// Every substitution (anything but an exact hit) is logged once and kept in fontSubstitutions().
#include <string>
#include <vector>
#include <map>
#include <set>
#include <algorithm>
#include <filesystem>
#include <cctype>
#include <functional>
#include <cstdio>

namespace dna {

struct FontSubstitution { std::string requested, used, how; };

namespace fontdetail {
inline std::string norm(std::string s) {
  auto sl = s.find_last_of("/\\"); if (sl != std::string::npos) s = s.substr(sl + 1);
  auto dot = s.find_last_of('.');
  if (dot != std::string::npos) {
    std::string ext = s.substr(dot + 1); for (auto &c : ext) c = (char)tolower(c);
    if (ext == "bam" || ext == "egg" || ext == "ttf" || ext == "otf" || ext == "pz") s = s.substr(0, dot);
  }
  std::string o; for (char c : s) if (isalnum((unsigned char)c)) o += (char)tolower((unsigned char)c);
  return o;
}
inline std::string stripSuffixes(std::string s) {
  static const char *suf[] = {"regular", "maximum", "standard", "classic", "bold", "font"};
  for (bool again = true; again;) {
    again = false;
    for (auto x : suf) { std::string t(x);
      if (s.size() > t.size() && s.compare(s.size() - t.size(), t.size(), t) == 0) { s.resize(s.size() - t.size()); again = true; } }
  }
  return s;
}
inline size_t lev(const std::string &a, const std::string &b) {
  std::vector<size_t> p(b.size() + 1), c(b.size() + 1);
  for (size_t j = 0; j <= b.size(); ++j) p[j] = j;
  for (size_t i = 1; i <= a.size(); ++i) { c[0] = i;
    for (size_t j = 1; j <= b.size(); ++j) c[j] = std::min({p[j] + 1, c[j - 1] + 1, p[j - 1] + (a[i - 1] != b[j - 1])});
    std::swap(p, c); }
  return p[b.size()];
}
struct Index {
  std::map<std::string, std::string> byNorm;   // normalized short name -> resource-relative path
  bool built = false;
  std::vector<std::string> roots;   // resource roots (the model path), set by the renderer
};
// Log sink (the renderer points it at core::warn); stderr until then.
inline std::function<void(const std::string &)> &logSink() {
  static std::function<void(const std::string &)> f = [](const std::string &m) { fprintf(stderr, "%s\n", m.c_str()); };
  return f;
}
inline Index &index() { static Index ix; return ix; }
inline std::vector<FontSubstitution> &subs() { static std::vector<FontSubstitution> v; return v; }
// Extension preference when one short name exists in several formats: the game's loader uses bam first.
inline int extRank(const std::string &p) {
  auto e = p.substr(p.find_last_of('.') + 1);
  return e == "bam" ? 0 : e == "egg" ? 1 : e == "ttf" ? 2 : 3;
}
inline void build() {
  auto &ix = index(); if (ix.built) return; ix.built = true;
  namespace fs = std::filesystem;
  for (auto &r : ix.roots) {
    fs::path root(r); std::error_code ec;
    if (!fs::is_directory(root, ec)) continue;
    for (auto &ph : fs::directory_iterator(root, ec)) {
      if (!ph.is_directory() || ph.path().filename().string().rfind("phase_", 0) != 0) continue;
      for (auto sub : {"fonts", "models/fonts"}) {
        fs::path d = ph.path() / sub; if (!fs::is_directory(d, ec)) continue;
        for (auto &f : fs::directory_iterator(d, ec)) {
          std::string ext = f.path().extension().string(); for (auto &c : ext) c = (char)tolower(c);
          if (ext != ".bam" && ext != ".egg" && ext != ".ttf" && ext != ".otf") continue;
          std::string rel = ph.path().filename().string() + "/" + sub + "/" + f.path().filename().string();
          std::string k = norm(rel);
          auto it = ix.byNorm.find(k);
          if (it == ix.byNorm.end() || extRank(rel) < extRank(it->second)) ix.byNorm[k] = rel;
        }
      }
    }
  }
  logSink()("sign fonts: indexed " + std::to_string(ix.byNorm.size()) + " fonts from the phase font folders");
}
// Codes and game globals -> short font names, as the game maps them (TTLocalizerEnglish / ToontownGlobals).
inline const std::map<std::string, std::string> &aliases() {
  static const std::map<std::string, std::string> a = {
    {"mickey", "MickeyFontMaximum"}, {"sign", "MickeyFontMaximum"}, {"signfont", "MickeyFontMaximum"},
    {"buildingnametagfont", "MickeyFontMaximum"}, {"classic", "MickeyFontStandard"}, {"mickeyfont", "MickeyFontMaximum"},
    {"humanist", "Humanist"}, {"toonfont", "ImpressBT"}, {"interface", "ImpressBT"}, {"interfacefont", "ImpressBT"}, {"impact", "ImpressBT"},
    {"fancy", "Comedy"}, {"fancyfont", "Comedy"}, {"ttcomedy", "Comedy"},
    {"suit", "vtRemingtonPortable"}, {"suitfont", "vtRemingtonPortable"}, {"cog", "vtRemingtonPortable"},
    {"minnie", "MinnieFont"}, {"ttminnie", "MinnieFont"}, {"minniefont", "MinnieFont"}};
  return a;
}
// Variants the game uses by default, preferred when several fonts share a stripped name.
inline int prefRank(const std::string &shortNorm) {
  static const char *pref[] = {"mickeyfontmaximum", "mickeyfontstandard", "mickeyfontclassic"};
  for (int i = 0; i < 3; ++i) if (shortNorm == pref[i]) return i;
  return 10;
}
inline std::string defaultSignFont() {
  auto &ix = index(); auto it = ix.byNorm.find("mickeyfontmaximum");
  if (it != ix.byNorm.end()) return it->second;
  return ix.byNorm.empty() ? "" : ix.byNorm.begin()->second;
}
inline void record(const std::string &req, const std::string &used, const std::string &how) {
  for (auto &s : subs()) if (s.requested == req) return;
  subs().push_back({req, used, how});
  logSink()("font substitution: '" + req + "' -> " + (used.empty() ? "editor default" : used) + " (" + how + ")");
}
}  // namespace fontdetail

// Resolve any font reference (storage path, code, short name) to a resource-relative font file.
// Returns "" only when no fonts exist at all.
inline std::string resolveSignFont(const std::string &requested) {
  using namespace fontdetail;
  build();
  static std::map<std::string, std::string> cache;
  static const void *cacheFor = nullptr;
  if (cacheFor != &index().roots || index().byNorm.empty()) { cache.clear(); cacheFor = &index().roots; }
  auto c = cache.find(requested); if (c != cache.end()) return c->second;
  auto &ix = index();
  std::string key = norm(requested), result, how;
  // hood codes like "DD_Portago" name the font after the underscore
  std::string hoodKey;
  { std::string base = requested; auto sl = base.find_last_of("/\\"); if (sl != std::string::npos) base = base.substr(sl + 1);
    auto us = base.find('_'); if (us != std::string::npos && us + 1 < base.size()) hoodKey = norm(base.substr(us + 1)); }
  // Game aliases win over a same-named file unless the request names an explicit file extension
  // (e.g. "MickeyFont" means the game's sign font, MickeyFontMaximum; "MickeyFont.ttf" means that file).
  bool hasExt = false;
  { auto sl = requested.find_last_of("/\\"); std::string b = sl == std::string::npos ? requested : requested.substr(sl + 1);
    auto d = b.find_last_of('.'); if (d != std::string::npos) { std::string e = b.substr(d + 1); for (auto &ch : e) ch = (char)tolower(ch);
      hasExt = e == "bam" || e == "egg" || e == "ttf" || e == "otf"; } }
  auto exact = ix.byNorm.find(key);
  if (!hasExt && aliases().count(key) && ix.byNorm.count(norm(aliases().at(key)))) {
    result = ix.byNorm[norm(aliases().at(key))];
    if (norm(result) != key) how = "game alias";
  }
  else if (exact != ix.byNorm.end()) result = exact->second;   // exact short-name hit: not a substitution
  else if (!hoodKey.empty() && ix.byNorm.count(hoodKey)) { result = ix.byNorm[hoodKey]; how = "hood font code"; }
  else {
    std::string sk = stripSuffixes(key); int best = 1 << 30;
    for (auto &[k, p] : ix.byNorm) if (stripSuffixes(k) == sk && prefRank(k) < best) { best = prefRank(k); result = p; how = "same name, other variant"; }
    if (result.empty() && sk.size() >= 3) {
      best = 1 << 30;
      for (auto &[k, p] : ix.byNorm) {
        std::string ks = stripSuffixes(k);
        if (ks.rfind(sk, 0) == 0 || sk.rfind(ks, 0) == 0 || k.find(sk) != std::string::npos || (ks.size() >= 4 && sk.find(ks) != std::string::npos)) {
          int score = prefRank(k) * 1000 + (int)k.size();
          if (score < best) { best = score; result = p; how = "prefix/contains match"; }
        }
      }
    }
    if (result.empty() && !sk.empty()) {
      size_t bestD = std::max<size_t>(2, sk.size() / 3) + 1;
      for (auto &[k, p] : ix.byNorm) { size_t d = lev(sk, stripSuffixes(k)); if (d < bestD) { bestD = d; result = p; how = "similar name (edit distance " + std::to_string(d) + ")"; } }
    }
    if (result.empty()) { result = defaultSignFont(); how = "no match -> default"; }
  }
  if (!how.empty()) record(requested, result, how);
  cache[requested] = result;
  return result;
}

// Give the resolver the resource roots to index (call before the first lookup).
inline void setFontRoots(const std::vector<std::string> &roots) {
  auto &ix = fontdetail::index();
  if (ix.built && ix.roots == roots) return;
  ix = {}; ix.roots = roots;
}

inline const std::vector<FontSubstitution> &fontSubstitutions() { return fontdetail::subs(); }

// Kept for callers that want the path a code resolves to.
inline std::string fallbackSignFontPath(const std::string &code) { return code.empty() ? "" : resolveSignFont(code); }

// Display name for the face a code resolves to.
inline std::string fallbackSignFontName(const std::string &code) {
  if (code.empty()) return "default";
  std::string p = resolveSignFont(code);
  if (p.empty()) return "none (editor default font)";
  auto s = p.find_last_of('/'); auto e = p.find_last_of('.');
  return p.substr(s + 1, e == std::string::npos || e < s ? std::string::npos : e - s - 1);
}

}  // namespace dna
