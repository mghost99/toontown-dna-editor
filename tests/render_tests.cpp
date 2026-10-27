// Scene generation checks run without a window or game assets.
#include "render/scene.h"
#include "pandaFramework.h"
#include "config_putil.h"
#include <filesystem>
#include <cstdio>

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
  printf("storage font registration and reset passed\n");
  return 0;
}
