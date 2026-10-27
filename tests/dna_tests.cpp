// Engine-free tests for the dna module: .dna parse/edit/save round trip, .xml <-> .dna conversion,
// .pdna compile/decompile, encrypted-.pdna password flow (with a stand-in cipher). Run: dna_tests SAMPLES_DIR TMP_DIR
#include "dna/dna_document.h"
#include "dna/dna_formats.h"
#include "commands/commands.h"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace fs = std::filesystem;
static int fails = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static int findItem(const dna::Document &d, const std::string &name) {
  for (size_t i = 0; i < d.items.size(); i++) if (d.items[i].name == name) return (int)i;
  return -1;
}
static bool load(dna::Document &d, const std::string &p) { std::string e; bool ok = d.load(p, fs::path(p).parent_path().string(), &e); if (!ok) printf("  load %s: %s\n", p.c_str(), e.c_str()); return ok; }
static bool save(dna::Document &d, const std::string &p) { std::string e; dna::SaveStats s; bool ok = d.save(&e, &s, p); if (!ok) printf("  save %s: %s\n", p.c_str(), e.c_str()); return ok; }

int main(int argc, char **argv) {
  if (argc < 3) { printf("usage: dna_tests SAMPLES_DIR TMP_DIR\n"); return 2; }
  std::string S = argv[1], T = argv[2]; fs::create_directories(T);
  std::string street = S + "/sample_street.dna", storage = S + "/sample_storage.dna";

  // 1. parse
  int perr = -1; std::string src = dna::readFile(street);
  auto tree = dna::parseDnaText(src, &perr);
  CHECK(perr == 0);
  dna::Document d; CHECK(load(d, street));
  CHECK(d.fmt == dna::Format::Dna);
  CHECK(d.items.size() == 7);
  CHECK(d.visgroups.size() == 1 && d.visgroups[0] == "9101");
  int tree0 = findItem(d, "prop_sample_tree_DNARoot"), lamp = findItem(d, "prop_sample_lamp_DNARoot");
  int lm = findItem(d, "tb3:toon_landmark_sample_DNARoot"), nested = findItem(d, "prop_nested_DNARoot");
  CHECK(tree0 >= 0 && lamp >= 0 && lm >= 0 && nested >= 0);
  CHECK(d.items[tree0].code == "prop_sample_tree" && d.items[tree0].xf.x == 5 && d.items[tree0].xf.h == 90 && d.items[tree0].xf.sx == 1.25);
  CHECK(!d.items[nested].nested && d.items[nested].visgroup == "9101");   // under a plain node, not an editable
  { const dna::XmlNode *t = nullptr; dna::walk(d.root.get(), [&](const dna::XmlNode *n) { if (n->tag == "title") t = n; });
    CHECK(t && dna::nodeText(t, d.text) == "Sample Shop"); }
  { const dna::XmlNode *t = nullptr; dna::walk(d.root.get(), [&](const dna::XmlNode *n) { if (n->tag == "text") t = n; });
    CHECK(t && dna::nodeText(t, d.text) == "SAMPLE"); }

  // 2. move + delete + add, save as .dna: comments and untouched text kept
  d.items[tree0].xf.x = 7.5; d.items[tree0].xf.h = 180;
  d.items[lamp].deleted = true;
  { dna::Item a; a.kind = "prop"; a.name = "prop_added_DNARoot"; a.code = "prop_sample_lamp"; a.visgroup = "9101";
    a.templateXml = "<prop name=\"prop_added_DNARoot\" code=\"prop_sample_lamp\">\n\t<pos x=\"0.00\" y=\"0.00\" z=\"0.00\" />\n\t<nhpr h=\"0.00\" p=\"0.00\" r=\"0.00\" />\n</prop>";
    a.xf.x = 3; a.xf.y = 4; d.items.push_back(a); }
  std::string edited = T + "/edited.dna";
  CHECK(save(d, edited));
  std::string et = dna::readFile(edited);
  CHECK(et.find("// a lone tree; the test moves it") != std::string::npos);
  CHECK(et.find("/* block comment") == std::string::npos);   // (that one lives in the storage sample)
  CHECK(et.find("pos [ 7.50 10 0 ]") != std::string::npos);
  CHECK(et.find("hpr [ 180.00 0 0 ]") != std::string::npos);
  CHECK(et.find("prop_sample_lamp_DNARoot") == std::string::npos);
  CHECK(et.find("prop \"prop_added_DNARoot\"") != std::string::npos);
  CHECK(et.find("cell_id [ 0 ]") != std::string::npos);
  dna::Document d2; CHECK(load(d2, edited));
  CHECK(d2.items.size() == 7);
  int t2 = findItem(d2, "prop_sample_tree_DNARoot"), a2 = findItem(d2, "prop_added_DNARoot");
  CHECK(t2 >= 0 && std::fabs(d2.items[t2].xf.x - 7.5) < 1e-9 && d2.items[t2].xf.h == 180);
  CHECK(a2 >= 0 && d2.items[a2].xf.x == 3 && d2.items[a2].xf.y == 4);
  // unchanged save is byte-identical apart from the header line
  { dna::Document d3; load(d3, street); std::string o = T + "/same.dna"; save(d3, o);
    std::string a = dna::readFile(o); a = a.substr(a.find('\n') + 1); CHECK(a == src); }

  // 3. .dna -> .xml -> .dna keeps the scene
  { dna::Document x; load(x, street); CHECK(save(x, T + "/conv.xml"));
    dna::Document xx; CHECK(load(xx, T + "/conv.xml")); CHECK(xx.fmt == dna::Format::Xml && xx.items.size() == 7);
    int i = findItem(xx, "prop_sample_tree_DNARoot"); CHECK(i >= 0 && xx.items[i].xf.x == 5 && xx.items[i].xf.sx == 1.25);
    CHECK(save(xx, T + "/conv_back.dna"));
    dna::Document xd; CHECK(load(xd, T + "/conv_back.dna")); CHECK(xd.items.size() == 7);
    std::string a, b; dna::compilePdna(xd.root.get(), xd.text, &a, nullptr); dna::compilePdna(x.root.get(), x.text, &b, nullptr);
    CHECK(a == b); }   // same compiled content after xml round trip

  // 4. .pdna compile / decompile / recompile is stable
  std::string p1 = T + "/street.pdna", p2 = T + "/street2.pdna";
  { dna::Document x; load(x, street); CHECK(save(x, p1)); }
  { std::string b = dna::readFile(p1); CHECK(b.compare(0, 7, std::string("PDNA\n\0\n", 7)) == 0); }
  { dna::Document x; CHECK(load(x, p1)); CHECK(x.fmt == dna::Format::Pdna && x.items.size() == 7);
    int i = findItem(x, "prop_sample_tree_DNARoot"); CHECK(i >= 0 && x.items[i].xf.x == 5 && x.items[i].xf.h == 90);
    x.items[i].xf.x = 9; CHECK(save(x, p2)); }
  { dna::Document x; CHECK(load(x, p2)); int i = findItem(x, "prop_sample_tree_DNARoot"); CHECK(i >= 0 && x.items[i].xf.x == 9);
    std::string a = T + "/street3.pdna"; x.items[i].xf.x = 5; save(x, a);
    CHECK(dna::readFile(a) == dna::readFile(p1)); }

  // 5. storage sample (models, textures, fonts) parses and compiles
  { std::string ss; auto st = dna::parseFile(storage, &ss); int nodes = 0, tex = 0, fonts = 0;
    dna::walk(st.get(), [&](const dna::XmlNode *n) { nodes += n->tag == "store_node"; tex += n->tag == "store_texture"; fonts += n->tag == "store_font"; });
    CHECK(nodes == 4 && tex == 2 && fonts == 1);
    std::string b, e; CHECK(dna::compilePdna(st.get(), ss, &b, &e));
    std::string txt; CHECK(dna::decompilePdna(b, &txt, &e));
    auto back = dna::parseDnaText(txt); std::string b2; dna::compilePdna(back.get(), txt, &b2, &e); CHECK(b == b2); }

  // 6. encrypted .pdna flow with a stand-in cipher (the app installs Panda3D's encrypt_string/decrypt_string)
  {
    auto x = [](const std::string &in, const std::string &pw, std::string *out) {
      *out = in; for (size_t i = 0; i < out->size(); i++) (*out)[i] ^= (char)(pw[i % pw.size()] + 0x5A); return true; };
    dna::setCipher(x, x);
    std::string plain = dna::readFile(p1), enc; x(plain, "hunter2", &enc);
    std::string ep = T + "/enc.pdna"; dna::writeFile(ep, enc);
    dna::setSessionPassword("");
    dna::Document e1; std::string err; CHECK(!e1.load(ep, T, &err)); CHECK(e1.loadError == dna::LoadError::NeedPassword);
    dna::setSessionPassword("wrong"); dna::Document e2; CHECK(!e2.load(ep, T, &err)); CHECK(e2.loadError == dna::LoadError::WrongPassword);
    dna::setSessionPassword("hunter2"); dna::Document e3; CHECK(e3.load(ep, T, &err)); CHECK(e3.encrypted && e3.items.size() == 7);
    CHECK(save(e3, T + "/enc2.pdna")); CHECK(dna::readFile(T + "/enc2.pdna") == enc);   // re-encrypted on save
    dna::setSessionPassword("");
  }

  // 7. Two compiled sign-text layouts: Stride (fixed point, the line stored on the baseline)
  // and Corporate Clash (the same sign line, with pos/hpr as float64).
  {
    auto le16 = [](std::string &b, int v) { b.push_back((char)(v & 255)); b.push_back((char)((v >> 8) & 255)); };
    auto le32 = [](std::string &b, int v) { uint32_t u = (uint32_t)v; for (int i = 0; i < 4; i++) b.push_back((char)((u >> (8 * i)) & 255)); };
    auto str = [&](std::string &b, const char *s) { le16(b, (int)std::strlen(s)); b.append(s); };
    auto f64 = [](std::string &b, double v) { uint64_t u; std::memcpy(&u, &v, 8); for (int i = 0; i < 8; i++) b.push_back((char)((u >> (8 * i)) & 255)); };
    auto f32 = [](std::string &b, float v) { uint32_t u; std::memcpy(&u, &v, 4); for (int i = 0; i < 4; i++) b.push_back((char)((u >> (8 * i)) & 255)); };
    auto color = [](std::string &b) { b.append(4, (char)255); };
    auto storage = [&](std::string &b) { for (int i = 0; i < 9; i++) le16(b, 0); };
    std::string body, txt, e;
    storage(body);
    body.push_back(1); str(body, "root");
    body.push_back(6); str(body, "");
    le32(body, 0); le32(body, 0); le32(body, -84); le32(body, 0); le32(body, 0); le32(body, 200);
    le16(body, 100); le16(body, 100); le16(body, 100);
    str(body, "HI"); str(body, "mickey"); color(body); str(body, "");
    for (int i = 0; i < 7; i++) f32(body, 0);
    body.push_back((char)255); body.push_back((char)255);
    std::string bytes = std::string("PDNA\n\0\n", 7) + body;
    CHECK(dna::decompilePdna(bytes, &txt, &e));
    CHECK(txt.find("letters [ \"HI\" ]") != std::string::npos);
    CHECK(txt.find("code [ \"mickey\" ]") != std::string::npos);
    CHECK(txt.find("pos [ 0 0 -0.84 ]") != std::string::npos);
    body.clear(); storage(body);
    body.push_back(1); str(body, "root");
    body.push_back(4); str(body, "npc");
    f64(body, -120.156); f64(body, 71.977); f64(body, 0.525); f64(body, 0); f64(body, 0); f64(body, 0);
    le16(body, 100); le16(body, 100); le16(body, 100);
    str(body, "DCS"); color(body);
    body.push_back((char)255); body.push_back((char)255);
    bytes = std::string("PDNA\n\0\n", 7) + body;
    CHECK(dna::decompilePdna(bytes, &txt, &e));
    CHECK(txt.find("-120.156") != std::string::npos);
    CHECK(txt.find("71.977") != std::string::npos);
    CHECK(txt.find("code [ \"DCS\" ]") != std::string::npos);
  }
  // Editing sign lines in every format preserves geometry, logos, pending
  // item commands and UTF-8 characters through save/reopen.
  for (const auto &extension : {"dna", "xml", "pdna"}) {
    dna::Document source; CHECK(load(source, street));
    std::string input = T + "/sign-edit." + extension;
    CHECK(save(source, input));
    dna::Document doc; CHECK(load(doc, input));
    auto baseline = [&]() { const dna::XmlNode *found = nullptr;
      dna::walk(doc.root.get(), [&](const dna::XmlNode *n) { if (n->tag == "baseline") found = n; }); return found; };
    doc.items[0].xf.x += 7; doc.items[1].deleted = true;
    dna::Item added; added.kind = "prop"; added.code = "new-prop"; added.name = "added";
    added.templateXml = "<prop name=\"added\" code=\"new-prop\"><pos x=\"2\" y=\"0\" z=\"0\" /></prop>";
    added.xf.x = 2; doc.items.push_back(added);
    const size_t count = doc.items.size(); const double moved = doc.items[0].xf.x;
    commands::History history;
    auto command = std::make_unique<commands::SourceCmd>();
    command->before = doc.text; command->beforeDirty = doc.textDirty;
    CHECK(doc.setSignLine(baseline(), "Café & Shop"));
    command->after = doc.text; history.pushApplied(std::move(command));
    CHECK(history.undo(doc)); CHECK(!doc.textDirty && doc.items.back().origId == -1);
    CHECK(history.redo(doc)); CHECK(doc.textDirty && doc.items.back().xf.x == 2);
    CHECK(doc.items.size() == count && doc.items.back().origId == -1);
    CHECK(doc.items[0].xf.x == moved && doc.items[1].deleted);
    CHECK(baseline() && baseline()->child("graphic"));
    CHECK(doc.setNodeAttribute(baseline(), "code", "custom_font"));
    CHECK(baseline()->attr("code") == "custom_font");
    const dna::XmlNode *title = nullptr;
    dna::walk(doc.root.get(), [&](const dna::XmlNode *n) { if (n->tag == "title") title = n; });
    CHECK(title && doc.setNodeText(title, "Edited Shop & Co"));
    CHECK(doc.setNodeAttribute(baseline(), "width", "42"));
    CHECK(save(doc, T + "/sign-result." + extension));
    dna::Document back; CHECK(load(back, T + "/sign-result." + extension));
    std::string words; int glyphs = 0;
    dna::walk(back.root.get(), [&](const dna::XmlNode *n) { if (n->tag == "text") { words += dna::nodeText(n, back.text); ++glyphs; } });
    CHECK(words == "Café & Shop" && glyphs == 11);
    CHECK(findItem(back, "added") >= 0 && back.items.size() == count - 1);
    CHECK(doc.setSignLine(baseline(), "Say \"hi\" \\ bye"));
    CHECK(save(doc, T + "/quoted." + extension));
    dna::Document quoted; CHECK(load(quoted, T + "/quoted." + extension));
    std::string quotedText;
    dna::walk(quoted.root.get(), [&](const dna::XmlNode *n) { if (n->tag == "text") quotedText += dna::nodeText(n, quoted.text); });
    CHECK(quotedText == "Say \"hi\" \\ bye");
    CHECK(doc.setSignLine(baseline(), ""));
    CHECK(baseline()->child("graphic") && !baseline()->child("text"));
    CHECK(doc.setSignLine(baseline(), "New"));
    CHECK(baseline()->child("text"));
  }
  {
    dna::Document doc; doc.fmt = dna::Format::Xml;
    CHECK(doc.replaceSource(0, 0, "<scene><prop name=\"sample\" code=\"sample\"><sign><baseline code=\"mickey\" /></sign></prop></scene>"));
    const dna::XmlNode *baseline = nullptr;
    dna::walk(doc.root.get(), [&](const dna::XmlNode *n) { if (n->tag == "baseline") baseline = n; });
    CHECK(doc.setSignLine(baseline, "Empty sign"));
    std::string text;
    dna::walk(doc.root.get(), [&](const dna::XmlNode *n) { if (n->tag == "text") text += dna::nodeText(n, doc.text); });
    CHECK(text == "Empty sign");
    dna::Item placed; placed.kind = "prop"; placed.templateXml = "<prop name=\"before\" code=\"a\" />";
    doc.items.push_back(placed); int id = (int)doc.items.size() - 1;
    commands::History history;
    auto command = std::make_unique<commands::TemplateCmd>(); command->id = id;
    command->before = placed.templateXml; command->after = "<prop name=\"after\" code=\"b\" />";
    history.execute(doc, std::move(command)); CHECK(doc.items[id].name == "after" && doc.items[id].code == "b");
    CHECK(history.undo(doc) && doc.items[id].name == "before");
    CHECK(history.redo(doc) && doc.items[id].name == "after");
  }
  {
    std::string bytes, error;
    auto empty = dna::parseDnaText("");
    CHECK(dna::compilePdna(empty.get(), "", &bytes, &error));
    std::string input = T + "/empty_storage.pdna"; CHECK(dna::writeFile(input, bytes));
    dna::Document doc; CHECK(load(doc, input));
    CHECK(save(doc, T + "/empty_storage.xml"));
    CHECK(save(doc, T + "/empty_storage.dna"));
    CHECK(save(doc, T + "/empty_storage_copy.pdna"));
    CHECK(dna::readFile(T + "/empty_storage_copy.pdna") == bytes);
  }
  printf("%d/%d checks passed\n", checks - fails, checks);
  return fails ? 1 : 0;
}
