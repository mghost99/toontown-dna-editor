#include "animation.h"
#include "storage.h"
#include "auto_bind.h"
#include "animControlCollection.h"
#include "animBundleNode.h"
#include "character.h"
#include "partBundle.h"
#include "sequenceNode.h"
#include "core/log.h"
#include <filesystem>

namespace assets {
namespace fs = std::filesystem;
void stopAnimations(std::vector<PT(AnimControl)> &controls) {
  for (auto &control : controls) control->stop();
  controls.clear();
}
std::vector<PT(AnimControl)> loopModel(NodePath model, const std::string &modelPath, const std::string &animation) {
  std::vector<PT(AnimControl)> controls;
  if (model.is_empty()) return controls;
  auto sequences = model.find_all_matches("**/+SequenceNode");
  for (int i = 0; i < sequences.get_num_paths(); ++i) DCAST(SequenceNode, sequences[i].node())->loop(true);
  AnimControlCollection embedded;
  auto_bind(model.node(), embedded, ~0);
  for (int i = 0; i < embedded.get_num_anims(); ++i) {
    auto control = embedded.get_anim(i);
    if (animation.empty() || animation == embedded.get_anim_name(i)) { control->loop(true); controls.push_back(control); }
  }
  if (!controls.empty()) return controls;
  auto characters = model.find_all_matches("**/+Character");
  if (model.node()->is_of_type(Character::get_class_type())) characters.add_path(model);
  if (characters.get_num_paths() == 0) return controls;
  fs::path directory = fs::path(modelPath).parent_path();
  std::vector<std::string> candidates;
  if (!animation.empty()) {
    candidates.push_back((directory / animation).generic_string());
    candidates.push_back(animation);
  } else {
    // Standard Toontown animated props use matching -mod/-chan or _r_/_a_
    // files. Prefer idle animations for a palette preview.
    std::string base = fs::path(modelPath).stem().string();
    if (auto i = base.find("_r_"); i != std::string::npos) {
      base.replace(i, 3, "_a_");
      for (const char *suffix : {"_idle0", "_idle", "_neutral", "_idleHiccup0"}) candidates.push_back((directory / (base + suffix)).generic_string());
    }
    if (base.size() > 4 && base.substr(base.size() - 4) == "-mod") candidates.push_back((directory / (base.substr(0, base.size() - 4) + "-chan")).generic_string());
  }
  std::string chosen;
  for (auto path : candidates) {
    if (fs::path(path).extension().empty()) path += ".bam";
    std::error_code ec;
    fs::path absolute = fs::path(path).is_absolute() ? fs::path(path) : fs::path(ModelCache::get().resRoot()) / path;
    if (fs::is_regular_file(absolute, ec)) { chosen = path; break; }
  }
  if (chosen.empty()) {
    if (!animation.empty()) core::warn("animation file not found for " + modelPath + ": " + animation);
    return controls;
  }
  NodePath anim = ModelCache::get().load(chosen);
  if (anim.is_empty()) return controls;
  auto bundles = anim.find_all_matches("**/+AnimBundleNode");
  if (anim.node()->is_of_type(AnimBundleNode::get_class_type())) bundles.add_path(anim);
  for (int i = 0; i < characters.get_num_paths(); ++i) {
    auto character = DCAST(Character, characters[i].node());
    for (int j = 0; j < bundles.get_num_paths(); ++j) {
      auto bundle = DCAST(AnimBundleNode, bundles[j].node())->get_bundle();
      PT(AnimControl) control = character->get_bundle(0)->bind_anim(bundle,
        PartGroup::HMF_ok_wrong_root_name | PartGroup::HMF_ok_part_extra | PartGroup::HMF_ok_anim_extra);
      if (control) { control->loop(true); controls.push_back(control); break; }
    }
  }
  return controls;
}
}
