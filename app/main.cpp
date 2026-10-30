// DNA Editor (native). Thin entry point: options, window, editor, optional --selftest.
#include "editor.h"
#include "core/log.h"
#include "core/resconfig.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "pnmImage.h"
#include "dna/dna_document.h"
#include "compress_string.h"
#include "encrypt_string.h"
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

[[maybe_unused]] static std::string findRes(const char *argv0) {
  std::vector<fs::path> c = {fs::current_path() / "resources"};
  fs::path p = fs::weakly_canonical(fs::path(argv0)).parent_path();
  for (int i = 0; i < 6 && !p.empty(); i++, p = p.parent_path()) c.push_back(p / "resources");
  for (auto &x : c) if (fs::is_directory(x / "phase_4")) return x.string();
  return "";
}

// mean/stddev of the luminance inside a rect: a blank (cleared-only) view has ~0 deviation
static double rectStd(const std::string &png, const render::Rect &r) {
  PNMImage img; if (!img.read(Filename::from_os_specific(png))) return -1;
  double s = 0, s2 = 0; int n = 0;
  for (int y = (int)r.y + 40; y < (int)(r.y + r.h) - 4; y += 3)
    for (int x = (int)r.x + 4; x < (int)(r.x + r.w) - 4; x += 3) {
      if (x < 0 || y < 0 || x >= img.get_x_size() || y >= img.get_y_size()) continue;
      double l = img.get_bright(x, y); s += l; s2 += l * l; n++;
    }
  if (!n) return -1; double m = s / n; return std::sqrt(std::max(0.0, s2 / n - m * m));
}

// zlib (compressed .pdna) and the password cipher (encrypted .pdna) come from Panda3D; dna/ stays engine-free.
static void installCodecs() {
#ifdef HAVE_ZLIB
  dna::setZlib([](const std::string &in, std::string *out) { *out = decompress_string(in); return !out->empty() || in.empty(); },
               [](const std::string &in, std::string *out) { *out = compress_string(in, 9); return !out->empty(); });
#endif
#ifdef HAVE_OPENSSL
  dna::setCipher(
      [](const std::string &in, const std::string &pw, std::string *out) {
        vector_uchar v; v.insert(v.end(), (const unsigned char *)in.data(), (const unsigned char *)in.data() + in.size()); *out = decrypt_string(v, pw); return !out->empty();
      },
      [](const std::string &in, const std::string &pw, std::string *out) {
        vector_uchar v = encrypt_string(in, pw); out->assign(v.begin(), v.end()); return !out->empty();
      });
#endif
}

// --convert IN OUT: headless format conversion / re-save (xml, dna, pdna by extension). Exit 0 on success.
static int convertMain(const std::string &in, const std::string &out, bool encryptOut) {
  dna::Document d; std::string err; dna::SaveStats s;
  fs::path ip = fs::absolute(in);
  if (!d.load(ip.string(), ip.parent_path().string(), &err)) {
    fprintf(stderr, "load failed: %s%s\n", err.c_str(),
            d.loadError == dna::LoadError::NeedPassword ? " (use --password or DNAED_PDNA_PASSWORD)" : "");
    return d.loadError == dna::LoadError::NeedPassword || d.loadError == dna::LoadError::WrongPassword ? 3 : 1;
  }
  if (encryptOut) {
    if (fs::path(out).extension() != ".pdna" || !dna::haveSessionPassword()) { fprintf(stderr, "--encrypt needs a .pdna output and --password/DNAED_PDNA_PASSWORD\n"); return 1; }
    d.encrypted = true;   // applies when writing .pdna
  }
  if (!d.save(&err, &s, fs::absolute(out).string())) { fprintf(stderr, "save failed: %s\n", err.c_str()); return 1; }
  std::error_code ec; fs::remove(fs::absolute(out).string() + ".bak", ec);
  printf("%s (%s, %d editable objects) -> %s", in.c_str(), dna::formatName(d.fmt), (int)d.items.size(), out.c_str());
  if (d.lastCompile.skipped) printf(" [skipped %d non-DNA elements: %s]", d.lastCompile.skipped, d.lastCompile.skippedTags.c_str());
  printf("\n");
  return 0;
}

int main(int argc, char **argv) {
  installCodecs();
  if (const char *pw = getenv("DNAED_PDNA_PASSWORD")) if (*pw) dna::setSessionPassword(pw);
  std::string res, file, selftest, snapPath; platform::Options o; int preSel = -1; int preOp = -1;
  bool doSave = false;
  bool previewEffects = false, animateBats = false; float dpos[3] = {0, 0, 0}; bool haveDpos = false;
  float dscale = 0; bool haveDscale = false;
  std::string convIn, convOut; bool encryptOut = false;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--res" && i + 1 < argc) res = argv[++i];
    else if (a == "--selftest" && i + 1 < argc) selftest = argv[++i];
    else if (a == "--encrypt") encryptOut = true;
    else if (a == "--password" && i + 1 < argc) dna::setSessionPassword(argv[++i]);
    else if (a == "--convert" && i + 2 < argc) { convIn = argv[i + 1]; convOut = argv[i + 2]; i += 2; }
    else if (a == "--offscreen") o.offscreen = true;
    else if (a == "--no-vsync") o.vsync = false;
    else if (a == "--select" && i + 1 < argc) preSel = atoi(argv[++i]);
    else if (a == "--gizmo" && i + 1 < argc) preOp = atoi(argv[++i]);
    else if (a == "--delta-pos" && i + 1 < argc) {
      haveDpos = sscanf(argv[++i], "%f,%f,%f", &dpos[0], &dpos[1], &dpos[2]) >= 1;
    } else if (a == "--delta-scale" && i + 1 < argc) {
      haveDscale = sscanf(argv[++i], "%f", &dscale) >= 1;
    } else if (a == "--save") doSave = true;
    else if (a == "--snap-and-exit" && i + 1 < argc) snapPath = argv[++i];
    else if (a == "--preview-effects") previewEffects = true;
    else if (a == "--animate-bats") animateBats = true;
    else if (a == "--size" && i + 1 < argc) sscanf(argv[++i], "%dx%d", &o.width, &o.height);
    else if (a == "-h" || a == "--help") {
      printf("usage: dna-editor [--res RESOURCES_DIR (else DNAED_RES, config.ini, auto)] [--size WxH] [--offscreen] [--selftest OUT_DIR] [--password PW (else DNAED_PDNA_PASSWORD)] [--convert IN OUT [--encrypt]] [DNA_FILE (.xml/.dna/.pdna)]\n"); return 0;
    } else file = a;
  }
  if (!convIn.empty()) return convertMain(convIn, convOut, encryptOut);
  double t0 = core::nowMs();
  std::string resSrc;
  res = core::resolveRes(res, argv[0], &resSrc);
  bool noRes = res.empty() || !fs::is_directory(res);
  if (!noRes) res = fs::canonical(res).string();
  {   // window icon: next to the exe (installed) or the repo's branding folder
    std::error_code ec; fs::path ex = fs::weakly_canonical(fs::path(argv[0]), ec).parent_path();
#ifdef _WIN32
    const char *names[] = {"dna-editor.ico", "../../assets/branding/dna-editor.ico", "../assets/branding/dna-editor.ico"};
#else
    const char *names[] = {"dna-editor.png", "../../assets/branding/dna-editor_64.png", "../share/icons/hicolor/64x64/apps/dna-editor.png"};
#endif
    for (auto n : names) if (fs::exists(ex / n, ec)) { o.icon = fs::weakly_canonical(ex / n, ec).string(); break; }
  }
  platform::App app;
  if (!app.open(1, argv, o)) return 1;
  ui::applyTheme(app.uiScale());
  ImGui::GetIO().IniFilename = nullptr;
  ImGui::GetIO().ConfigWindowsMoveFromTitleBarOnly = true;   // layout is code-defined (View > Reset panel layout)
  if (noRes) {   // no resources yet: ask for the folder before loading anything
    res = platform::pickFolder("Choose your game resources folder");
    if (!res.empty() && core::isResourcesDir(res)) { core::writeConfigRes(res); resSrc = "picked"; }
    else { fprintf(stderr, "no resources folder: pass --res, set DNAED_RES, or pick one (config %s)\n", core::configPath().c_str()); app.close(); return 2; }
  }
  Editor ed(app, res);
  core::setTiming("startup (window + catalog)", core::nowMs() - t0);
  core::info("resources: " + res + " (" + resSrc + ", config " + core::configPath() + ")");
  // no file given: reopen the last opened file (View > Reopen last file on startup; config reopen_last=0 turns it off)
  if (file.empty() && selftest.empty() && core::readConfig("reopen_last") != "0") {
    std::string last = core::readConfig("last_file"); std::error_code ec;
    if (!last.empty() && fs::is_regular_file(last, ec)) { core::info("reopening last file: " + last); file = last; }
  }
  if (!file.empty()) ed.open(file);
  core::setTiming("startup to first file open", core::nowMs() - t0);

  // DNAED_SELECT_NAME=a,b selects items by name (comma list) for snapshots
  if (const char *nm = getenv("DNAED_SELECT_NAME")) {
    std::string list = std::string(",") + nm + ",";
    std::vector<int> sel;
    for (int i = 0; i < (int)ed.doc.items.size(); i++)
      if (list.find("," + ed.doc.items[i].name + ",") != std::string::npos) sel.push_back(i);
    if (!sel.empty()) { ed.selection = sel; if (getenv("DNAED_FRAME_SEL")) ed.frameSelection(); preSel = -1; }
  }
  if (preSel >= 0 && preSel < (int)ed.doc.items.size()) { ed.selection = {preSel}; if (getenv("DNAED_FRAME_SEL")) ed.frameSelection(); }
  if (const char *cam = getenv("DNAED_CAM")) {   // close-up snapshots: "dist[,yaw[,pitch]]" around the framed target
    float d = ed.v3->dist, y = ed.v3->yaw, p = ed.v3->pitch;
    if (sscanf(cam, "%f,%f,%f", &d, &y, &p) >= 1) { ed.v3->dist = d; ed.v3->yaw = y; ed.v3->pitch = p; ed.v3->apply(); }
  }
  if (const char *ca = getenv("DNAED_CONTEXT_AREA")) ed.setContextArea(ca);   // e2e: pick a View > Playground context area
  if (const char *pz = getenv("DNAED_PASTE")) {   // e2e: Ctrl+C the selection, Ctrl+V at copied centre + "dx,dy,dz[,zone]"
    float dx = 0, dy = 0, dz = 0; char mode[16] = {0};
    sscanf(pz, "%f,%f,%f,%15s", &dx, &dy, &dz, mode);
    ed.copySelected();
    for (int id : ed.selection) { NodePath n = ed.scene.itemNodes[id]; LPoint3 w = n.get_pos(ed.scene.world); printf("copy [%d] parent=%s world (%.2f,%.2f,%.2f)\n", id, n.get_parent().get_name().c_str(), w[0], w[1], w[2]); }
    LPoint3 at = ed.clipboardCenter() + LVector3(dx, dy, dz);
    int n = ed.pasteClipboard(std::string(mode) == "zone", &at);
    for (int id : ed.selection) { auto &x = ed.doc.items[id].xf;
      printf("pasted [%d] %s xf (%.3f,%.3f,%.3f) h%.1f p%.1f s%.3f\n", id, ed.doc.items[id].name.c_str(), x.x, x.y, x.z, x.h, x.p, x.sx); }
    printf("paste %d\n", n);
  }
  if (preOp >= 0) ed.gizmoOp = (render::GizmoOp)preOp;
  if (haveDpos && preSel >= 0 && preSel < (int)ed.doc.items.size()) {
    // Simulate a Move-gizmo nudge on the selected item, then optional save (e2e / smoke).
    auto &it = ed.doc.items[preSel];
    printf("delta-pos on [%d] %s: (%.3f,%.3f,%.3f) -> ", preSel, it.name.c_str(), it.xf.x, it.xf.y, it.xf.z);
    it.xf.x += dpos[0]; it.xf.y += dpos[1]; it.xf.z += dpos[2];
    ed.doc.textDirty = true;
    ed.scene.syncItem(ed.doc, preSel);
    printf("(%.3f,%.3f,%.3f)\n", it.xf.x, it.xf.y, it.xf.z);
  }
  if (haveDscale && preSel >= 0 && preSel < (int)ed.doc.items.size()) {
    auto &it = ed.doc.items[preSel];
    printf("delta-scale on [%d] %s: (%.3f,%.3f,%.3f) -> ", preSel, it.name.c_str(), it.xf.sx, it.xf.sy, it.xf.sz);
    double f = dscale;
    it.xf.sx = std::max(0.01, it.xf.sx * f);
    it.xf.sy = std::max(0.01, it.xf.sy * f);
    it.xf.sz = std::max(0.01, it.xf.sz * f);
    ed.doc.textDirty = true;
    ed.scene.syncItem(ed.doc, preSel);
    printf("(%.3f,%.3f,%.3f)\n", it.xf.sx, it.xf.sy, it.xf.sz);
  }
  if (doSave) {
    bool ok = ed.save("");
    printf("save %s\n", ok ? "ok" : "FAILED");
    if (!ok) { app.close(); return 1; }
  }
  if (!snapPath.empty()) {   // real-window smoke test: render a few frames, screenshot, exit
    if (previewEffects) ed.setPreviewEffects(true);
    if (animateBats) ed.setAnimateBats(true);
    if (getenv("DNAED_SNAP_NOSEL")) ed.selection.clear();   // hide the selection box/gizmo
    int nFrames = previewEffects ? 48 : 8;   // enough for odd/even flash cycle
    for (int i = 0; i < nFrames; i++) app.step([&] { ed.frame(); });
    bool ok = app.screenshot(snapPath);
    printf("snap %s: %s\n", snapPath.c_str(), ok ? "ok" : "FAILED");
    { auto r = ed.v3->rect(); printf("view3d rect %.0f %.0f %.0f %.0f\n", (double)r.x, (double)r.y, (double)r.w, (double)r.h); }
    app.close(); return ok ? 0 : 1;
  }
  if (selftest.empty()) {
    app.run([&] { ed.frame(); }, [&] { return ed.quit; });
    app.close();
    return 0;
  }

  // ---------------- self test: drives the real UI loop, takes screenshots, checks views never go blank ----------------
  fs::create_directories(selftest);
  app.injectInput = true;
  ImGuiIO &io = ImGui::GetIO();
  int fails = 0, shotN = 0;
  std::ofstream rep(selftest + "/report.txt");
  auto frames = [&](int n) { for (int i = 0; i < n; i++) app.step([&] { ed.frame(); }); };
  auto shot = [&](const std::string &name, bool check3d = true, bool check2d = true) {
    frames(2);
    char b[256]; snprintf(b, sizeof b, "%s/%02d_%s.png", selftest.c_str(), ++shotN, name.c_str());
    app.screenshot(b);
    double s3 = rectStd(b, ed.v3->rect()), s2 = rectStd(b, ed.v2->rect());
    bool ok = (!check3d || s3 > 0.02) && (!check2d || s2 > 0.02);
    if (!ok) fails++;
    rep << (ok ? "OK   " : "FAIL ") << b << "  3D-view std " << s3 << "  2D-map std " << s2 << "\n"; rep.flush();
    return ok;
  };
  auto mouse = [&](float x, float y, int btn, int down) {   // down: -1 move only, 0 up, 1 down
    io.AddMousePosEvent(x, y); if (down >= 0) io.AddMouseButtonEvent(btn, down == 1); frames(1);
  };
  auto editField = [&](const std::string &id, const std::string &value) {
    auto fields = ui::inspectorFields;
    auto field = std::find_if(fields.begin(), fields.end(), [&](const ui::InspectorField &field) { return field.id == id; });
    if (field == fields.end()) return false;
    for (auto window : ImGui::GetCurrentContext()->Windows) {
      if (std::string(window->Name).find("Inspector/insp-scroll") != std::string::npos) {
        if (field->y < window->InnerClipRect.Min.y || field->y > window->InnerClipRect.Max.y)
          ImGui::SetScrollY(window, window->Scroll.y + field->y - (window->InnerClipRect.Min.y + window->InnerClipRect.Max.y) / 2);
      }
    }
    frames(3); fields = ui::inspectorFields;
    field = std::find_if(fields.begin(), fields.end(), [&](const ui::InspectorField &field) { return field.id == id; });
    if (field == fields.end()) return false;
    mouse(field->x, field->y, 0, -1); mouse(field->x, field->y, 0, 1); mouse(field->x, field->y, 0, 0);
    io.AddKeyEvent(ImGuiMod_Ctrl, true); io.AddKeyEvent(ImGuiKey_A, true); frames(1);
    io.AddKeyEvent(ImGuiKey_A, false); io.AddKeyEvent(ImGuiMod_Ctrl, false); frames(1);
    io.AddInputCharactersUTF8(value.c_str()); frames(2);
    io.AddKeyEvent(ImGuiKey_Tab, true); frames(1); io.AddKeyEvent(ImGuiKey_Tab, false); frames(3);
    return true;
  };
  frames(3);
  // Fractional wheel input must remain fine even on a large map.
  {
    auto rectangle = ed.v3->rect();
    mouse(rectangle.x + rectangle.w * 0.5f, rectangle.y + rectangle.h * 0.5f, 0, -1);
    auto original = ed.v3->target;
    io.AddMouseWheelEvent(0, 0.1f); frames(2);
    float travel = (ed.v3->target - original).length();
    bool fine = std::fabs(travel - ed.wheelSpeed * 0.1f) < 0.02f;
    rep << "fractional wheel travel: " << travel << " ft, expected " << ed.wheelSpeed * 0.1f << "\n";
    if (!fine) ++fails;
    float distance = ed.v3->dist; ed.v3->dist = 10000;
    original = ed.v3->target;
    io.AddMouseWheelEvent(0, 0.1f); frames(2);
    travel = (ed.v3->target - original).length();
    if (std::fabs(travel - ed.wheelSpeed * 0.1f) > 0.02f) ++fails;
    ed.v3->dist = distance; ed.frameAll();
  }
  std::vector<std::string> files = {"phase_4/dna/toontown_central_sz.xml", "phase_5/dna/toontown_central_2100.xml"};
  if (!file.empty()) files = {file};
  for (auto &f : files) {
    std::string tag = fs::path(f).stem().string();
    double ot = core::nowMs(); ed.open(f); rep << "open " << f << ": " << core::nowMs() - ot << " ms\n";
    ed.setLight(render::LightMode::Game); shot(tag + "_open");
    // Drive sign text and font edits through the Inspector's actual widgets.
    int signItem = -1;
    for (int i = 0; i < (int)ed.doc.items.size() && signItem < 0; ++i) {
      if (!ed.doc.items[i].node) continue;
      dna::walk(ed.doc.items[i].node, [&](const dna::XmlNode *node) {
        if (node->tag == "baseline" && node->child("text")) signItem = i;
      });
    }
    if (signItem >= 0) {
      ed.select({signItem}); ed.frameSelection(); frames(3);
      std::string before = ed.doc.text;
      bool edited = editField("sign:0", "Editor Test");
      bool persisted = ed.doc.text != before;
      rep << "Inspector text input commits: " << (edited && persisted ? "yes" : "NO") << "\n";
      if (!edited || !persisted) ++fails;
      ed.select({signItem}); frames(2);
      edited = editField("font:0", "mickey");
      bool font = false;
      dna::walk(ed.doc.items[signItem].node, [&](const dna::XmlNode *node) { if (node->tag == "baseline" && node->attr("code") == "mickey") font = true; });
      if (!edited || !font) ++fails;
      shot(tag + "_inspector_sign");
      while (ed.history.canUndo()) ed.history.undo(ed.doc);
      if (ed.doc.text != before) ++fails;
      rep << "Inspector text/font edits undo to original source: " << (ed.doc.text == before ? "yes" : "NO") << "\n";
      ed.frameAll(); ed.select({}); frames(2);
    }

    ed.setLight(render::LightMode::CogMood); shot(tag + "_cogmood"); ed.setLight(render::LightMode::Game);
    // select: click the 3D view where an object projects
    int target = -1; float tx = 0, ty = 0;
    for (int i = 0; i < (int)ed.doc.items.size() && target < 0; i++) {
      auto &it = ed.doc.items[i]; if (it.nested || it.kind == "treasure_spawn") continue;
      LPoint3 a, b; NodePath n = ed.scene.itemNodes[i];
      if (!n.calc_tight_bounds(a, b, ed.scene.world)) continue;
      float x, y; auto r = ed.v3->rect();
      if (ed.v3->project((a + b) * 0.5f, x, y) && x > r.x + 60 && x < r.x + r.w - 60 && y > r.y + 60 && y < r.y + r.h - 60) {
        double pt = core::nowMs(); int e = ed.pickAt3D(x, y); double dt = core::nowMs() - pt;
        if (e >= 0) { target = e; tx = x; ty = y; rep << "pick: " << dt << " ms -> item " << e << "\n"; }
      }
    }
    if (target < 0) { rep << "no pickable object in view for " << f << "\n"; continue; }
    ed.gizmoOp = render::GizmoOp::None;
    mouse(tx, ty, 0, -1); mouse(tx, ty, 0, 1); mouse(tx, ty, 0, 0);
    rep << "selected via click: " << (ed.selection.size() ? std::to_string(ed.selection[0]) : "none") << " (wanted " << ed.doc.items[target].name << ")\n";
    if (ed.selection.empty()) ed.select({target});
    int sel = ed.selection[0];
    auto before = ed.doc.items[sel].xf;
    shot(tag + "_selected");
    // real mouse drag in the 3D view (free drag at current height), 3 times
    for (int k = 0; k < 3; k++) {
      double dt = core::nowMs();
      LPoint3 a, b; ed.scene.itemNodes[sel].calc_tight_bounds(a, b, ed.scene.world); float x, y; ed.v3->project((a + b) * 0.5f, x, y);
      mouse(x, y, 0, -1); mouse(x, y, 0, 1);
      for (int s = 1; s <= 12; s++) mouse(x + s * 6, y + s * 2, 0, -1);
      mouse(x + 72, y + 24, 0, 0);
      rep << "3D drag " << k << ": " << (core::nowMs() - dt) / 15 << " ms/frame, item now at " << ed.doc.items[sel].xf.x << "," << ed.doc.items[sel].xf.y << "\n";
      shot(tag + "_drag3d_" + std::to_string(k));
    }
    // 2D map drag
    { float x, y; ed.v2->project(ed.scene.itemNodes[sel].get_pos(ed.scene.world), x, y);
      mouse(x, y, 0, -1); mouse(x, y, 0, 1); for (int s = 1; s <= 10; s++) mouse(x - s * 4, y + s * 4, 0, -1); mouse(x - 40, y + 40, 0, 0);
      rep << "2D drag: item now at " << ed.doc.items[sel].xf.x << "," << ed.doc.items[sel].xf.y << "\n"; shot(tag + "_drag2d"); }
    // gizmo: W translate along X, E rotate, R scale -- real mouse drags on the ImGuizmo handles
    for (int op = 1; op <= 3; op++) {
      ed.select({sel}); ed.gizmoOp = (render::GizmoOp)op; frames(2); if (op == 1) shot(tag + "_gizmo_shown");
      auto b4 = ed.doc.items[sel].xf; NodePath n = ed.scene.itemNodes[sel];
      float ox, oy, ax, ay; LPoint3 o = n.get_pos(ed.scene.world);
      ed.v3->project(o, ox, oy); ed.v3->project(o + LVector3(1, 0, 0) * (ed.v3->dist * 0.05f), ax, ay);
      float dx = ax - ox, dy = ay - oy, L = std::sqrt(dx * dx + dy * dy); if (L < 1e-3f) L = 1; dx /= L; dy /= L;
      bool moved = false;
      rep << "  gizmo origin px " << ox << "," << oy << " xdir " << dx << "," << dy << " rect h " << ed.v3->rect().h << "\n";
      for (float f : {0.85f, 0.7f, 0.95f, 0.55f, 1.05f, 1.15f, 1.25f}) {
        float d = ed.v3->rect().h * 0.09f * f;
        float px = ox + dx * d, py = oy + dy * d;
        if (op == 2) { px = ox + d * 0.707f; py = oy - d * 0.707f; }   // rotate: on a ring
        mouse(px, py, 0, -1); frames(1); mouse(px, py, 0, 1);
        for (int s = 1; s <= 8; s++) mouse(px + (op == 2 ? s * 6 : dx * s * 6), py + (op == 2 ? s * 6 : dy * s * 6), 0, -1);
        mouse(px + (op == 2 ? 48 : dx * 48), py + (op == 2 ? 48 : dy * 48), 0, 0); frames(1);
        if (ed.selection.empty()) ed.select({sel});
        if (ed.doc.items[sel].xf != b4) { moved = true; break; }
      }
      const char *nm[] = {"", "translate", "rotate", "scale"};
      rep << "gizmo " << nm[op] << ": " << (moved ? "changed transform" : "no change (handle not hit)") << "  h=" << ed.doc.items[sel].xf.h << " x=" << ed.doc.items[sel].xf.x << " sx=" << ed.doc.items[sel].xf.sx << "\n";
      { auto &a = b4, &b = ed.doc.items[sel].xf; rep << "    before pos " << a.x << "," << a.y << "," << a.z << " hpr " << a.h << "," << a.p << "," << a.r << " scale " << a.sx << "," << a.sy << "," << a.sz << "\n    after  pos " << b.x << "," << b.y << "," << b.z << " hpr " << b.h << "," << b.p << "," << b.r << " scale " << b.sx << "," << b.sy << "," << b.sz << "\n"; }
      shot(tag + "_gizmo_" + nm[op]);
    }
    ed.gizmoOp = render::GizmoOp::None;
    // 60 scripted moves/rotates/scales through the command path
    double mt = core::nowMs();
    for (int k = 0; k < 60; k++) {
      ed.select({sel});
      if (k % 3 == 0) ed.moveSelectedBy(LVector3(0.5f, -0.25f, 0), true);
      else { ed.beginEdit(); auto &x = ed.doc.items[sel].xf; if (k % 3 == 1) x.h += 7.5; else { x.sx *= 1.01; x.sy *= 1.01; x.sz *= 1.01; } ed.scene.syncItem(ed.doc, sel); ed.endEdit(k % 3 == 1 ? "Rotate" : "Scale"); }
      frames(1);
      if (k % 20 == 19) shot(tag + "_moves_" + std::to_string(k + 1));
    }
    rep << "60 edits+frames: " << (core::nowMs() - mt) / 60 << " ms each\n";
    int n = 0; while (ed.history.undo(ed.doc)) { n++; if (n % 25 == 0) frames(1); }
    bool back = ed.doc.items[sel].xf == before;
    rep << "undo " << n << " steps -> original transform restored: " << (back ? "yes" : "NO") << "\n"; if (!back) fails++;
    shot(tag + "_undone");
    int m = 0; while (ed.history.redo(ed.doc)) m++;
    rep << "redo " << m << " steps\n"; shot(tag + "_redone");
    ed.duplicateSelected(); shot(tag + "_duplicated"); ed.deleteSelected(); shot(tag + "_dup_deleted");
    // palette preview + place
    int ci = 0; for (int i = 0; i < (int)ed.catalog.size(); i++) if (ed.catalog[i].code.find("pp_") == 0 || ed.catalog[i].code.find("prop_") == 0) { ci = i; break; }
    bool animatedPreview = false;
    for (const auto &item : ed.doc.items) {
      if (!item.node || item.node->attr("anim").empty()) continue;
      for (int i = 0; i < (int)ed.catalog.size(); ++i) if (ed.catalog[i].code == item.code) { ci = i; animatedPreview = true; break; }
      if (animatedPreview) break;
    }
    double pt = core::nowMs(); ed.openPreview(ci); frames(1); rep << "preview open (" << ed.catalog[ci].code << "): " << core::nowMs() - pt << " ms\n";
    if (animatedPreview) {
      rep << "scene animation controls: " << ed.scene.animations.size() << ", preview controls: " << ed.previewAnimationCount() << "\n";
      if (ed.scene.animations.empty() || ed.previewAnimationCount() == 0) ++fails;
    }
    ImGui::SetWindowFocus("Prop Preview"); shot(tag + "_preview"); ImGui::SetWindowFocus("Objects");
    int pid = ed.placePreviewed(); rep << "placed item " << pid << "\n"; shot(tag + "_placed");
    ed.select({pid}); frames(2);
    bool renamed = editField("property:" + ed.doc.items[pid].kind + ":name", "placed_editor_test");
    bool renameOk = renamed && ed.doc.items[pid].name == "placed_editor_test";
    rep << "Inspector edits newly placed object: " << (renameOk ? "yes" : "NO") << "\n";
    if (!renameOk) ++fails;
    if (renameOk) {
      ed.history.undo(ed.doc); if (ed.doc.items[pid].name == "placed_editor_test") ++fails;
      ed.history.redo(ed.doc); if (ed.doc.items[pid].name != "placed_editor_test") ++fails;
    }
    // save to a scratch dir (never the repo file) and re-parse
    std::string sd = selftest + "/saved"; bool ok = ed.save(sd);
    dna::Document chk; std::string err; bool lo = chk.load(sd + "/" + fs::path(f).filename().string(), res, &err);
    int live = 0; for (auto &it : ed.doc.items) live += !it.deleted;
    rep << "save to " << sd << ": " << (ok ? "ok" : "FAILED") << ", reparse " << (lo ? "ok" : err) << ", items " << chk.items.size() << " (editor has " << live << " live)\n";
    if (!ok || !lo || (int)chk.items.size() != live) fails++;
    shot(tag + "_after_save");
    // Enter and exit walking with the real shortcut; movement and jump also
    // use ImGui key events, so focus and key routing are exercised.
    {
      auto rect = ed.v3->rect(); mouse(rect.x + rect.w / 2, rect.y + rect.h / 2, 0, -1);
      mouse(rect.x + rect.w / 2, rect.y + rect.h / 2, 0, 1); mouse(rect.x + rect.w / 2, rect.y + rect.h / 2, 0, 0);
      LPoint3 camera = ed.v3->cam.get_pos();
      io.AddKeyEvent(ImGuiKey_F5, true); frames(1); io.AddKeyEvent(ImGuiKey_F5, false); frames(2);
      bool active = ed.walker.active(); rep << "F5 starts Toon walker: " << (active ? "yes" : "NO") << "\n";
      if (active) {
        LPoint3 start = ed.walker.root.get_pos();
        io.AddKeyEvent(ImGuiKey_UpArrow, true); frames(15); io.AddKeyEvent(ImGuiKey_UpArrow, false); frames(1);
        bool moved = (ed.walker.root.get_pos() - start).length() > 0.1f;
        rep << "arrow input moves Toon: " << (moved ? "yes" : "NO") << "\n"; if (!moved) ++fails;
        float z = ed.walker.root.get_z(); io.AddKeyEvent(ImGuiMod_Ctrl, true); frames(3);
        bool jumped = ed.walker.root.get_z() > z + 0.05f;
        io.AddKeyEvent(ImGuiMod_Ctrl, false); frames(1);
        rep << "Ctrl input jumps: " << (jumped ? "yes" : "NO") << "\n"; if (!jumped) ++fails;
        shot(tag + "_walker");
        io.AddKeyEvent(ImGuiKey_F5, true); frames(1); io.AddKeyEvent(ImGuiKey_F5, false); frames(2);
        if (ed.walker.active() || (ed.v3->cam.get_pos() - camera).length() > 0.01f) ++fails;
      } else ++fails;
    }

  }
  {
    auto rect = ed.v3->rect();
    mouse(rect.x + 20, rect.y + 45, 0, -1); mouse(rect.x + 20, rect.y + 45, 0, 1); mouse(rect.x + 20, rect.y + 45, 0, 0);
  }
  io.AddKeyEvent(ImGuiKey_F2, true); frames(1); io.AddKeyEvent(ImGuiKey_F2, false); frames(2);
  auto help = ImGui::FindWindowByName("Shortcuts"); if (!help || !help->Active) ++fails;
  shot("shortcuts", false, false);
  io.AddKeyEvent(ImGuiKey_F2, true); frames(1); io.AddKeyEvent(ImGuiKey_F2, false); frames(1);
  if (file.empty()) {   // in-place save on a scratch copy: .bak written, file re-read, edit persisted
    std::string dir = selftest + "/inplace"; fs::create_directories(dir);
    std::string cp = dir + "/toontown_central_sz.xml";
    fs::copy_file(res + "/phase_4/dna/toontown_central_sz.xml", cp, fs::copy_options::overwrite_existing);
    ed.open(cp); int id = -1;
    for (int i = 0; i < (int)ed.doc.items.size(); i++) if (ed.doc.items[i].kind == "prop") { id = i; break; }
    ed.select({id}); ed.moveSelectedBy(LVector3(3, 0, 0), true); double want = ed.doc.items[id].xf.x;
    bool ok = ed.save(); bool bak = fs::exists(cp + ".bak");
    double got = ed.doc.items[id].xf.x; bool same = std::fabs(got - want) < 0.011;
    rep << "in-place save: " << (ok ? "ok" : "FAILED") << ", .bak " << (bak ? "written" : "MISSING") << ", reopened x " << got << " (wanted " << want << ")\n";
    if (!ok || !bak || !same) fails++;
    shot("inplace_after_save");
  }
  rep << "\nTIMINGS\n"; for (auto &[k, v] : core::timings()) if (k.rfind("load model", 0) != 0) rep << "  " << k << ": " << v << " ms\n";
  rep << (fails ? "\nRESULT: " + std::to_string(fails) + " failure(s)\n" : "\nRESULT: all checks passed\n");
  rep.close();
  printf("selftest %s, report: %s/report.txt\n", fails ? "FAILED" : "passed", selftest.c_str());
  app.close();
  return fails ? 1 : 0;
}
