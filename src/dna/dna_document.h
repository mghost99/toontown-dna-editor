// DNA scene document: editable items (props, buildings, treasure spawns...) with their transforms, visgroups,
// storage-file selection, the prop catalog, and the span-preserving save. Pure C++ (no Panda3D).
#pragma once
#include "dna_formats.h"
#include "xml_span.h"
#include <set>
#include <string>
#include <vector>

namespace dna {

extern const std::set<std::string> kEditable;   // prop, anim_prop, interactive_prop, treasure_spawn, *_building

struct Xform {
  double x = 0, y = 0, z = 0, h = 0, p = 0, r = 0, sx = 1, sy = 1, sz = 1;
  bool operator==(const Xform &o) const;
  bool operator!=(const Xform &o) const { return !(*this == o); }
};

struct Item {
  int origId = -1;            // index among editable elements of the file as loaded; -1 = added this session
  std::string kind, name, code, visgroup;
  double width = 0;
  bool nested = false;        // inside another editable (moves with its parent)
  bool deleted = false;
  Xform xf, origXf;
  std::string templateXml;    // for added items: element text to insert (transform patched at save)
  const XmlNode *node = nullptr;   // source element (origId >= 0)
};

struct SaveStats { int moved = 0, deleted = 0, added = 0; };

class Document {
 public:
  bool load(const std::string &absPath, const std::string &resRoot, std::string *err);
  std::string path, rel, resRoot, text;   // text: XML or DNA source (decompiled for .pdna)
  Format fmt = Format::Xml;               // format of the file on disk
  bool compressed = false;
  bool encrypted = false;                 // .pdna was password-encrypted (re-encrypted on save)
  bool textDirty = false;                 // sign lettering edited in memory
  LoadError loadError = LoadError::None;                // .pdna zlib flag as loaded (kept on save)
  CompileStats lastCompile;               // last .pdna write
  std::unique_ptr<XmlNode> root;
  std::vector<Item> items;
  std::vector<std::string> visgroups;
  int indexOfNode(const XmlNode *n) const;     // item index for an editable element node, -1 if none
  // Rewrites one baseline's per-letter text nodes from a single string. Reparses the file text.
  bool setNodeText(const XmlNode *node, const std::string &value);
  bool setItemTemplate(int index, const std::string &xml);
  bool setNodeAttribute(const XmlNode *node, const std::string &key, const std::string &value);
  bool replaceSource(size_t start, size_t end, const std::string &replacement);
  bool setSignLine(const XmlNode *baseline, const std::string &letters);
  bool dirty() const;
  // Builds the patched text (only changed tags rewritten). Pure; does not touch disk.
  std::string buildSave(SaveStats *stats) const;
  // Writes <file>.bak (once per save) then the patched text atomically. Returns false + err on failure.
  // outPath may change the format (by extension: .xml / .dna / .pdna) = Save As / export.
  bool save(std::string *err, SaveStats *stats, const std::string &outPath = "");
  // Element source of an item converted to XML (templates for duplicates / renderer are always XML).
  std::string itemXml(const Item &it) const;
};

std::string withHeader(const std::string &text, const std::string &rel);
std::string withDnaHeader(const std::string &text, const std::string &rel);
std::string fmtNum(double v);

// ---- storage selection (which storage_*.xml files a DNA file needs) ----
std::vector<std::string> storagesForFile(const std::string &resRoot, const std::string &rel, const std::string &text,
                                         std::vector<std::string> *undefinedCodes);
std::vector<std::string> sceneFiles(const std::string &resRoot);   // rel paths of non-storage DNA xmls

// ---- palette catalog ----
struct CatalogEntry { std::string code, root, model, node, storageRel; };
std::vector<CatalogEntry> loadCatalog(const std::string &resRoot);

}  // namespace dna
