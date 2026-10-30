// Builds Panda3D geometry for a DNA document using the game's DNA rules (port of toontown/dna/*.py), with one
// NodePath per editable item so edits only touch that node's transform -- the rest of the scene is never rebuilt.
#pragma once
#include "assets/storage.h"
#include "assets/animation.h"
#include "dna/dna_document.h"
#include "nodePath.h"
#include <map>
#include <string>
#include <vector>

namespace render {

class Scene {
 public:
  Scene();
  NodePath world;     // lights/fog attach here; render roots of 3D and 2D cameras
  NodePath geom;      // generated DNA
  NodePath markers;   // treasure spawn / empty markers
  std::vector<NodePath> itemNodes;                 // parallel to Document::items
  std::map<std::string, NodePath> visgroupNodes;

  void build(const dna::Document &doc, assets::Storage &st);
  void clear();
  // Generates a node for doc.items[i] (an added item) from its templateXml.
  void buildAdded(const dna::Document &doc, int i, assets::Storage &st);
  void syncItem(const dna::Document &doc, int i);   // transform + visibility from the item
  void setVisgroupVisible(const std::string &zone, bool v);
  LPoint3 boundsMin, boundsMax;
  void computeBounds();
  int stats_nodes = 0;
  std::vector<PT(AnimControl)> animations;
 private:
  void gen(const dna::XmlNode *n, NodePath parent, const std::string &src, assets::Storage &st,
           const std::map<const dna::XmlNode *, int> *ids, float *wallHeight, const dna::XmlNode *flat);
  std::map<std::string, int> unknownTags_;
  // Appearance Override ATTACH-TO-MODEL: parent_code -> world-placed holder
  std::map<std::string, NodePath> attachParents_;
  NodePath ensureAttachParent(const std::string &code, assets::Storage &st,
                             const dna::Document *doc);
  void prepareAttachParents(const dna::Document &doc, assets::Storage &st);
};

NodePath makeMarker(const LColor &c, float size);   // small octahedron, unlit
}  // namespace render
