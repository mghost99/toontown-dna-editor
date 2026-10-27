// Scene generation checks run without a window or game assets.
#include "render/scene.h"
#include "pandaFramework.h"
#include "config_putil.h"
#include <filesystem>
#include <cstdio>
#include <cmath>
#include "clockObject.h"
#include "render/walker.h"
#include "collisionPlane.h"
#include "collisionNode.h"
#include <cstdlib>

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
  doc.text = "prop \"hidden\" [ code [ \"DCS\" ] scale [ 0 -2 0 ] ]";
  doc.root = dna::parseDnaText(doc.text); scene.build(doc, storage);
  NodePath hidden = scene.geom.find("**/hidden");
  if (hidden.is_empty() || hidden.get_sx() <= 0 || hidden.get_sz() <= 0 || hidden.get_sy() != -2) return 1;
  printf("storage fonts, curved text, glyph scale, per-letter properties and Unicode passed\n");
  if (const char *fixture = std::getenv("DNAED_TEST_OLD_PDNA")) {
    dna::Document old; std::string error;
    if (!old.load(fixture, std::filesystem::path(fixture).parent_path().string(), &error)) return 1;
    const dna::XmlNode *baked = nullptr;
    dna::walk(old.root.get(), [&](const dna::XmlNode *node) { if (node->tag == "baseline" && node->child("text_geom")) baked = node; });
    if (!baked) { printf("no baked sign fixture found\n"); return 1; }
    doc.text = dna::toXmlText(baked, old.text, 0); doc.root = dna::parseXml(doc.text);
    scene.build(doc, storage);
    if (scene.geom.find_all_matches("**/+GeomNode").is_empty()) { printf("baked sign did not generate geometry\n"); return 1; }
    printf("older PDNA baked sign geometry renders\n");
  }
  if (const char *resources = std::getenv("DNAED_TEST_RES")) {
    assets::ModelCache::get().setResRoot(resources);
    get_model_path().prepend_directory(Filename(resources));
    NodePath world("walker-test"), ground = world.attach_new_node("scene");
    PT(CollisionNode) floor = new CollisionNode("floor");
    floor->set_into_collide_mask(BitMask32(2)); floor->set_from_collide_mask(BitMask32::all_off());
    floor->add_solid(new CollisionPlane(LPlane(LVector3(0, 0, 1), LPoint3(0, 0, 0))));
    ground.attach_new_node(floor);
    render::Walker walker;
    if (!walker.start(world, ground, LPoint3(0, 0, 0)) || !walker.grounded) { printf("walker startup/grounding failed\n"); return 1; }
    for (int i = 0; i < 120; ++i) walker.step(ground, 1.f / 60, 1, 0, false);
    if (walker.root.get_y() < 20) { printf("walker forward motion failed\n"); return 1; }
    PT(CollisionNode) wall = new CollisionNode("wall");
    wall->set_into_collide_mask(BitMask32(1)); wall->set_from_collide_mask(BitMask32::all_off());
    wall->add_solid(new CollisionPlane(LPlane(LVector3(0, -1, 0), LPoint3(0, 26, 0))));
    ground.attach_new_node(wall);
    for (int i = 0; i < 120; ++i) walker.step(ground, 1.f / 60, 1, 0, false);
    if (walker.root.get_y() > 25.4f) { printf("walker wall collision failed\n"); return 1; }
    for (int i = 0; i < 60; ++i) walker.step(ground, 1.f / 60, 0, 1, false);
    if (walker.root.get_h() < 90) { printf("walker turning failed\n"); return 1; }
    walker.step(ground, 0.1f, 0, 0, true);
    if (walker.root.get_z() < 0.5f || walker.grounded) { printf("walker jump failed\n"); return 1; }
    for (int i = 0; i < 120; ++i) walker.step(ground, 1.f / 60, 0, 0, false);
    if (!walker.grounded || std::fabs(walker.root.get_z()) > 0.01f) { printf("walker landing failed\n"); return 1; }
    walker.stop(); if (walker.active()) return 1;
    printf("Toon assembly, walking, turning, jump and landing passed\n");
    const char *model = std::getenv("DNAED_TEST_ANIM_MODEL"), *animation = std::getenv("DNAED_TEST_ANIM_NAME");
    if (model && animation) {
      assets::ModelCache::get().setResRoot(resources);
      get_model_path().prepend_directory(Filename(resources));
      NodePath rig = assets::ModelCache::get().load(model);
      auto controls = assets::loopModel(rig, model, animation);
      if (controls.empty()) { printf("external animation failed to bind\n"); return 1; }
      auto clock = ClockObject::get_global_clock();
      clock->set_mode(ClockObject::M_non_real_time); clock->set_dt(0.1);
      double before = controls.front()->get_full_fframe();
      for (int i = 0; i < 10; ++i) clock->tick();
      if (controls.front()->get_full_fframe() <= before) return 1;
      assets::stopAnimations(controls);
      printf("external prop animation binds, advances and stops\n");
    }
  }
  return 0;
}
