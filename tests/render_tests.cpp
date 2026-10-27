// Scene generation checks run without a window or game assets.
#include "render/scene.h"
#include "pandaFramework.h"
#include "config_putil.h"
#include <filesystem>
#include <cstdio>
#include <cmath>

int main(int argc, char **argv) {
  PandaFramework framework; framework.open_framework(argc, argv);
  if (argc != 2) return 2;
  std::filesystem::create_directories(argv[1]);
  assets::ModelCache::get().setResRoot(argv[1]);
  dna::writeFile(std::string(argv[1]) + "/storage.dna",
    "store_font [ \"font\" \"custom\" \"my-font.ttf\" ]\n");
  assets::Storage storage;
  storage.addFiles({"storage.dna"});
  if (storage.fontPath("custom") != "my-font.ttf") return 1;
  storage.clear();
  if (!storage.fontPath("custom").empty()) return 1;
  dna::Document doc;
  doc.text = "baseline [ code [ \"\" ] scale [ 2 1 3 ] width [ 30 ] height [ 30 ] text [ letters [ \"ABC\" ] ] ]";
  doc.root = dna::parseDnaText(doc.text);
  render::Scene scene; scene.build(doc, storage);
  NodePath baseline = scene.geom.find("**/baseline"), line = scene.geom.find("**/typesetter");
  if (baseline.is_empty() || line.is_empty() || line.get_num_children() != 3) return 1;
  if (std::fabs(baseline.get_sx() - 1) > 0.001f || std::fabs(line.get_child(0).get_sx() - 2) > 0.001f) return 1;
  if (std::fabs(line.get_child(0).get_z() - line.get_child(2).get_z()) < 0.001f) return 1;
  if (std::fabs(line.get_child(0).get_r() - line.get_child(2).get_r()) < 0.001f) return 1;
  // Per-letter color and position survive rendering, including UTF-8 text.
  doc.text = "baseline [ text [ letters [ \"é\" ] pos [ 0 0 2 ] color [ 1 0 0 1 ] ] text [ letters [ \"B\" ] ] ]";
  doc.root = dna::parseDnaText(doc.text); scene.build(doc, storage);
  line = scene.geom.find("**/typesetter");
  if (line.get_num_children() != 2 || std::fabs(line.get_child(0).get_z() - 2) > 0.001f) return 1;
  if (!line.get_child(0).has_color_scale()) return 1;
  printf("storage fonts, curved text, glyph scale, per-letter properties and Unicode passed\n");
  return 0;
}
