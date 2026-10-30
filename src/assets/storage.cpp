#include "storage.h"
#include "core/log.h"
#include "dna/xml_span.h"
#include "dna/dna_formats.h"
#include "loader.h"
#include "loaderOptions.h"
#include "texturePool.h"
#include "modelRoot.h"
#include "fontPool.h"

namespace assets {

ModelCache &ModelCache::get() { static ModelCache c; return c; }

NodePath ModelCache::load(const std::string &p) {
  auto it = models_.find(p);
  if (it != models_.end()) return it->second;
  core::ScopedTimer t("load model " + p, false);
  LoaderOptions opts(LoaderOptions::LF_search | LoaderOptions::LF_report_errors | LoaderOptions::LF_no_disk_cache);
  PT(PandaNode) n = Loader::get_global_ptr()->load_sync(Filename(p), opts);
  // Game DNA often omits the extension; try .bam like the client model-path search.
  if (n == nullptr && p.find('.') == std::string::npos)
    n = Loader::get_global_ptr()->load_sync(Filename(p + ".bam"), opts);
  NodePath np;
  if (n == nullptr) core::warn("model not found or unreadable, skipped: " + p);
  else np = NodePath(n);
  models_[p] = np;
  return np;
}

PT(Texture) ModelCache::texture(const std::string &p) {
  auto it = tex_.find(p); if (it != tex_.end()) return it->second;
  PT(Texture) t = TexturePool::load_texture(Filename(p));
  if (t == nullptr) core::warn("texture not found, skipped: " + p);
  tex_[p] = t; return t;
}

void Storage::clear() { files_.clear(); nodes_.clear(); texPaths_.clear(); fontPaths_.clear(); resolved_.clear(); catalog_.clear(); missing.clear(); }

void Storage::addFiles(const std::vector<std::string> &rels) {
  auto &res = ModelCache::get().resRoot();
  for (auto &r : rels) {
    std::string t; auto x = dna::parseFile(res + "/" + r, &t);
    if (t.empty()) { core::error("storage file missing or unreadable: " + r); continue; }
    files_.push_back(r);
    dna::walk(x.get(), [&](const dna::XmlNode *n) {
      if (n->tag == "store_node") {
        NodeRef ref; ref.model = n->parent ? n->parent->attr("path") : "";
        std::string code = n->attr("code");
        ref.whole = n->attrs.count("node") && n->attr("node").empty();
        ref.node = n->attrs.count("node") ? n->attr("node") : code; ref.storage = r;
        if (code.empty()) return;
        nodes_[code] = ref; resolved_.erase(code);
        catalog_[n->attr("root")].push_back(code);
      } else if (n->tag == "store_font") {
        fontPaths_[n->attr("code")] = n->attr("path");
      } else if (n->tag == "store_texture") {
        texPaths_[n->attr("code")] = n->attr("path");
        catalog_[n->attr("root")].push_back(n->attr("code"));
      }
    });
  }
}

void Storage::addCode(const std::string &code, const std::string &model, const std::string &node, const std::string &st) {
  if (nodes_.count(code)) return;
  NodeRef r; r.model = model; r.node = node.empty() ? code : node; r.whole = node.empty(); r.storage = st;
  nodes_[code] = r; resolved_.erase(code); missing.erase(code);
}

NodePath Storage::findNode(const std::string &code) {
  if (code.empty()) return NodePath();   // e.g. a sign with no backing model: nothing to load, nothing missing
  auto r = resolved_.find(code); if (r != resolved_.end()) return r->second;
  auto it = nodes_.find(code);
  NodePath out;
  if (it == nodes_.end()) {
    if (code != "DCS" && missing.insert(code).second) core::warn("code not in any loaded storage file: " + code);
  } else {
    NodePath m = ModelCache::get().load(it->second.model);
    if (!m.is_empty()) {
      if (it->second.whole) out = m;
      else {
        out = m.find("**/" + it->second.node);
        if (out.is_empty()) core::warn("node '" + it->second.node + "' not found in " + it->second.model + " (code " + code + ")");
      }
    }
  }
  resolved_[code] = out;
  return out;
}

std::string Storage::fontPath(const std::string &code) const {
  auto it = fontPaths_.find(code);
  return it == fontPaths_.end() ? "" : it->second;
}
PT(TextFont) Storage::findFont(const std::string &code) {
  std::string path = fontPath(code);
  return path.empty() ? nullptr : FontPool::load_font(path);
}

PT(Texture) Storage::findTexture(const std::string &code) {
  auto it = texPaths_.find(code);
  if (it == texPaths_.end()) { if (missing.insert(code).second) core::warn("texture code not in storage: " + code); return nullptr; }
  return ModelCache::get().texture(it->second);
}
int Storage::numCatalog(const std::string &root) const { auto i = catalog_.find(root); return i == catalog_.end() ? 0 : (int)i->second.size(); }
std::string Storage::catalogCode(const std::string &root, int i) const { return catalog_.at(root).at(i); }

NodePath loadStoreNode(const std::string &model, const std::string &node) {
  NodePath m = ModelCache::get().load(model);
  if (m.is_empty() || node.empty()) return m;
  return m.find("**/" + node);
}

}  // namespace assets
