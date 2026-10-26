// DNA storage: code -> model node / texture, loaded lazily from the game's .bam/.egg files (exact game parity:
// same Panda3D loader). Bad or missing files are logged once and replaced by placeholders, never crash.
#pragma once
#include "nodePath.h"
#include "texture.h"
#include <map>
#include <set>
#include <string>
#include <vector>

namespace assets {

class ModelCache {
 public:
  static ModelCache &get();
  void setResRoot(const std::string &r) { res_ = r; }
  const std::string &resRoot() const { return res_; }
  NodePath load(const std::string &modelPath);   // empty NodePath on failure (logged once)
  PT(Texture) texture(const std::string &path);
  size_t loadedCount() const { return models_.size(); }
 private:
  std::string res_;
  std::map<std::string, NodePath> models_;
  std::map<std::string, PT(Texture)> tex_;
};

class Storage {
 public:
  struct NodeRef { std::string model, node; bool whole = false; std::string storage; };
  void clear();
  // Registers the store_node / store_texture entries of these storage files (no model loading yet).
  void addFiles(const std::vector<std::string> &rels);
  const std::vector<std::string> &files() const { return files_; }
  bool hasCode(const std::string &code) const { return nodes_.count(code) > 0; }
  NodePath findNode(const std::string &code);       // loads on first use; empty if unknown/bad
  PT(Texture) findTexture(const std::string &code);
  int numCatalog(const std::string &root) const;
  std::string catalogCode(const std::string &root, int i) const;
  std::set<std::string> codes() const { std::set<std::string> o; for (auto &kv : nodes_) o.insert(kv.first); return o; }
  // Adds one code from another storage file (palette placement) without overriding loaded ones.
  void addCode(const std::string &code, const std::string &model, const std::string &node, const std::string &storage);
  std::set<std::string> missing;                    // codes asked for but not defined
 private:
  std::vector<std::string> files_;
  std::map<std::string, NodeRef> nodes_;
  std::map<std::string, std::string> texPaths_;
  std::map<std::string, NodePath> resolved_;
  std::map<std::string, std::vector<std::string>> catalog_;
};

// Loads one model sub-node for previews (store_node semantics: node "" = whole model).
NodePath loadStoreNode(const std::string &model, const std::string &node);

}  // namespace assets
