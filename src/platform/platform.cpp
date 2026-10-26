#include "platform.h"
#include "core/log.h"
#include "imgui.h"
#include "backends/imgui_impl_opengl3.h"
#include "ImGuizmo.h"
#include "buttonThrower.h"
#include "callbackObject.h"
#include "displayRegion.h"
#include "displayRegionDrawCallbackData.h"
#include "graphicsWindow.h"
#include "graphicsPipe.h"
#include <algorithm>
#include "keyboardButton.h"
#include "mouseButton.h"
#include "pnmImage.h"
#include "throw_event.h"
#include "load_prc_file.h"
#include <filesystem>
#include <map>

#ifdef _WIN32
#include <windows.h>
#include <shobjidl.h>
#else
#include <cstdio>
#endif

namespace platform {

std::string pickFolder(const std::string &title, const std::string &start) {
#ifdef _WIN32
  std::string out;
  HRESULT ci = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  IFileOpenDialog *d = nullptr;
  if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) {
    DWORD f = 0; d->GetOptions(&f); d->SetOptions(f | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    std::wstring wt(title.begin(), title.end()); d->SetTitle(wt.c_str());
    if (!start.empty()) {
      int n = MultiByteToWideChar(CP_UTF8, 0, start.c_str(), -1, nullptr, 0); std::wstring ws(n, 0);
      MultiByteToWideChar(CP_UTF8, 0, start.c_str(), -1, ws.data(), n);
      IShellItem *si = nullptr; if (SUCCEEDED(SHCreateItemFromParsingName(ws.c_str(), nullptr, IID_PPV_ARGS(&si)))) { d->SetFolder(si); si->Release(); }
    }
    if (SUCCEEDED(d->Show(nullptr))) {
      IShellItem *it = nullptr;
      if (SUCCEEDED(d->GetResult(&it))) {
        PWSTR w = nullptr;
        if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &w))) {
          int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr); out.resize(n);
          WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), n, nullptr, nullptr); out.resize(n ? n - 1 : 0); CoTaskMemFree(w);
        }
        it->Release();
      }
    }
    d->Release();
  }
  if (SUCCEEDED(ci)) CoUninitialize();
  return out;
#else
  auto q = [](const std::string &v) { std::string r = "'"; for (char c : v) { if (c == '\'') r += "'\\''"; else r += c; } return r + "'"; };
  std::string cmds[] = {
      "zenity --file-selection --directory --title=" + q(title) + (start.empty() ? "" : " --filename=" + q(start + "/")) + " 2>/dev/null",
      "kdialog --getexistingdirectory " + q(start.empty() ? "." : start) + " --title " + q(title) + " 2>/dev/null"};
  for (auto &c : cmds) {
    std::string bin = c.substr(0, c.find(' '));
    if (system(("command -v " + bin + " >/dev/null 2>&1").c_str()) != 0) continue;
    FILE *p = popen(c.c_str(), "r"); if (!p) continue;
    char buf[4096]; std::string out; while (fgets(buf, sizeof buf, p)) out += buf; pclose(p);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
  }
  return "";
#endif
}

const std::vector<FileFilter> kDnaFilters = {
    {"DNA files (*.xml, *.dna, *.pdna)", "*.xml;*.dna;*.pdna"}, {"DNA XML (*.xml)", "*.xml"},
    {"DNA source (*.dna)", "*.dna"}, {"Compiled DNA (*.pdna)", "*.pdna"}, {"All files", "*"}};

std::string pickFile(const std::string &title, const std::string &start, bool save, const std::vector<FileFilter> &filters) {
#ifdef _WIN32
  auto wide = [](const std::string &s) { int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0); std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n); if (!w.empty()) w.pop_back(); return w; };
  std::string out;
  HRESULT ci = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  IFileDialog *d = nullptr;
  HRESULT hr = save ? CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS((IFileSaveDialog **)&d))
                    : CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS((IFileOpenDialog **)&d));
  if (SUCCEEDED(hr)) {
    std::vector<std::wstring> store; std::vector<COMDLG_FILTERSPEC> spec;
    for (auto &f : filters) { store.push_back(wide(f.label)); store.push_back(wide(f.pattern)); }
    for (size_t i = 0; i < filters.size(); i++) spec.push_back({store[2 * i].c_str(), store[2 * i + 1].c_str()});
    d->SetFileTypes((UINT)spec.size(), spec.data());
    DWORD fl = 0; d->GetOptions(&fl); d->SetOptions(fl | FOS_FORCEFILESYSTEM | (save ? FOS_OVERWRITEPROMPT : FOS_FILEMUSTEXIST));
    d->SetTitle(wide(title).c_str());
    if (!start.empty()) {
      std::filesystem::path sp(start); std::error_code ec;
      std::string folder = std::filesystem::is_directory(sp, ec) ? start : sp.parent_path().string();
      if (save && !std::filesystem::is_directory(sp, ec)) d->SetFileName(wide(sp.filename().string()).c_str());
      IShellItem *si = nullptr; if (SUCCEEDED(SHCreateItemFromParsingName(wide(folder).c_str(), nullptr, IID_PPV_ARGS(&si)))) { d->SetFolder(si); si->Release(); }
    }
    if (SUCCEEDED(d->Show(nullptr))) {
      IShellItem *it = nullptr;
      if (SUCCEEDED(d->GetResult(&it))) {
        PWSTR w = nullptr;
        if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &w))) {
          int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr); out.resize(n);
          WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), n, nullptr, nullptr); out.resize(n ? n - 1 : 0); CoTaskMemFree(w);
        }
        it->Release();
      }
    }
    d->Release();
  }
  if (SUCCEEDED(ci)) CoUninitialize();
  return out;
#else
  auto q = [](const std::string &v) { std::string r = "'"; for (char c : v) { if (c == '\'') r += "'\\''"; else r += c; } return r + "'"; };
  std::string zf, kf;
  for (auto &f : filters) {
    std::string pat = f.pattern; for (auto &ch : pat) if (ch == ';') ch = ' ';
    zf += " --file-filter=" + q(std::string(f.label) + " | " + pat);
    if (kf.empty()) kf = pat;
  }
  std::string cmds[] = {
      "zenity --file-selection" + std::string(save ? " --save --confirm-overwrite" : "") + " --title=" + q(title) + zf +
          (start.empty() ? "" : " --filename=" + q(start)) + " 2>/dev/null",
      std::string("kdialog ") + (save ? "--getsavefilename " : "--getopenfilename ") + q(start.empty() ? "." : start) + " " + q(kf) +
          " --title " + q(title) + " 2>/dev/null"};
  for (auto &c : cmds) {
    std::string bin = c.substr(0, c.find(' '));
    if (system(("command -v " + bin + " >/dev/null 2>&1").c_str()) != 0) continue;
    FILE *p = popen(c.c_str(), "r"); if (!p) continue;
    char buf[4096]; std::string out; while (fgets(buf, sizeof buf, p)) out += buf; pclose(p);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
  }
  return "";
#endif
}

static bool g_glInit = false;

class ImGuiDraw : public CallbackObject {
 public:
  ALLOC_DELETED_CHAIN(ImGuiDraw);
  void do_callback(CallbackData *) override {
    if (!g_glInit) { ImGui_ImplOpenGL3_Init("#version 130"); g_glInit = true; }
    ImDrawData *d = ImGui::GetDrawData();
    static bool skip = getenv("DNAED_NOIMGUI") != nullptr;
    if (d && d->Valid && !skip) ImGui_ImplOpenGL3_RenderDrawData(d);
  }
};

static ImGuiKey mapKey(const std::string &n) {
  static const std::map<std::string, ImGuiKey> m = {
      {"tab", ImGuiKey_Tab}, {"arrow_left", ImGuiKey_LeftArrow}, {"arrow_right", ImGuiKey_RightArrow},
      {"arrow_up", ImGuiKey_UpArrow}, {"arrow_down", ImGuiKey_DownArrow}, {"page_up", ImGuiKey_PageUp},
      {"page_down", ImGuiKey_PageDown}, {"home", ImGuiKey_Home}, {"end", ImGuiKey_End}, {"insert", ImGuiKey_Insert},
      {"delete", ImGuiKey_Delete}, {"backspace", ImGuiKey_Backspace}, {"space", ImGuiKey_Space},
      {"enter", ImGuiKey_Enter}, {"escape", ImGuiKey_Escape}, {"lcontrol", ImGuiKey_LeftCtrl},
      {"rcontrol", ImGuiKey_RightCtrl}, {"lshift", ImGuiKey_LeftShift}, {"rshift", ImGuiKey_RightShift},
      {"lalt", ImGuiKey_LeftAlt}, {"ralt", ImGuiKey_RightAlt}, {"f1", ImGuiKey_F1}, {"f2", ImGuiKey_F2},
      {"f3", ImGuiKey_F3}, {"f4", ImGuiKey_F4}, {"f5", ImGuiKey_F5}, {"f6", ImGuiKey_F6}, {"f12", ImGuiKey_F12},
      {"-", ImGuiKey_Minus}, {"=", ImGuiKey_Equal}, {",", ImGuiKey_Comma}, {".", ImGuiKey_Period},
      {"/", ImGuiKey_Slash}, {";", ImGuiKey_Semicolon}, {"[", ImGuiKey_LeftBracket}, {"]", ImGuiKey_RightBracket}};
  auto it = m.find(n); if (it != m.end()) return it->second;
  if (n.size() == 1) {
    char c = n[0];
    if (c >= 'a' && c <= 'z') return (ImGuiKey)(ImGuiKey_A + (c - 'a'));
    if (c >= '0' && c <= '9') return (ImGuiKey)(ImGuiKey_0 + (c - '0'));
  }
  return ImGuiKey_None;
}

static bool g_inLog = getenv("DNAED_INPUT_LOG") != nullptr;
InputStats g_input;
static void onButton(const Event *e, bool down) {
  if (e->get_num_parameters() < 1) return;
  std::string n = e->get_parameter(0).get_string_value();
  g_input.events++; g_input.last = n + (down ? " down" : " up");
  if (g_inLog) fprintf(stderr, "[input] %s %s\n", n.c_str(), down ? "down" : "up");
  ImGuiIO &io = ImGui::GetIO();
  if (n == "mouse1") { io.AddMouseButtonEvent(0, down); return; }
  if (n == "mouse3") { io.AddMouseButtonEvent(1, down); return; }
  if (n == "mouse2") { io.AddMouseButtonEvent(2, down); return; }
  if (n == "wheel_up") { if (down) io.AddMouseWheelEvent(0, 1); return; }
  if (n == "wheel_down") { if (down) io.AddMouseWheelEvent(0, -1); return; }
  if (n == "wheel_left") { if (down) io.AddMouseWheelEvent(1, 0); return; }
  if (n == "wheel_right") { if (down) io.AddMouseWheelEvent(-1, 0); return; }
  if (n == "lcontrol" || n == "rcontrol" || n == "control") io.AddKeyEvent(ImGuiMod_Ctrl, down);
  if (n == "lshift" || n == "rshift" || n == "shift") io.AddKeyEvent(ImGuiMod_Shift, down);
  if (n == "lalt" || n == "ralt" || n == "alt") io.AddKeyEvent(ImGuiMod_Alt, down);
  ImGuiKey k = mapKey(n); if (k != ImGuiKey_None) io.AddKeyEvent(k, down);
}
static void onDown(const Event *e, void *) { onButton(e, true); }
static void onUp(const Event *e, void *) { onButton(e, false); }
static void onKey(const Event *e, void *) {
  if (e->get_num_parameters() < 1) return;
  std::wstring w = e->get_parameter(0).get_wstring_value();   // ButtonThrower sends a 1-char wstring
  for (wchar_t ch : w) if (ch >= 32 && ch != 127) ImGui::GetIO().AddInputCharacter((unsigned)ch);
}

bool App::open(int argc, char **argv, const Options &o) {
  offscreen = o.offscreen;
  std::string prc = "window-title " + o.title + "\nwin-size " + std::to_string(o.width) + " " + std::to_string(o.height) +
                    "\nframebuffer-multisample 1\nmultisamples 4\nsync-video " + std::string(o.vsync ? "1" : "0") + "\naudio-library-name null\n"
                    "load-display pandagl\nnotify-level-glgsg error\nnotify-level-display error\ndefault-model-extension .bam\n"
                    "x-wm-class-name dna-editor\nx-wm-class dna-editor\nmodel-cache-dir\ntextures-power-2 none\ngl-coordinate-system default\nwin-fixed-size 0\n";
  if (o.offscreen) prc += "window-type offscreen\nsync-video 0\n";
  if (const char *extra = getenv("DNAED_PRC")) { prc += extra; prc += "\n"; }   // e.g. DNAED_PRC="framebuffer-multisample 0"
  if (!o.icon.empty()) prc += "icon-filename " + Filename::from_os_specific(o.icon).get_fullpath() + "\n";
  load_prc_file_data("editor", prc);
  fw.open_framework(argc, argv);
  fw.set_window_title(o.title);
  if (!o.offscreen) {   // fit the window on the screen (Panda is DPI-aware: sizes are physical pixels)
    GraphicsPipe *pipe = fw.get_default_pipe();
    if (pipe) {
      int dw = pipe->get_display_width(), dh = pipe->get_display_height();
      uiScale_ = std::max(1.0f, (float)pipe->get_display_zoom());
      int w = o.width, h = o.height;
      if (dw > 0 && dh > 0) { w = std::min(w, (int)(dw * 0.94f)); h = std::min(h, (int)(dh * 0.86f)); }
      core::info("display " + std::to_string(dw) + "x" + std::to_string(dh) + " zoom " + std::to_string(uiScale_) +
                 " -> window " + std::to_string(w) + "x" + std::to_string(h));
      load_prc_file_data("editor-fit", "win-size " + std::to_string(w) + " " + std::to_string(h) + "\nwin-origin -2 -2\n");
    }
  }
  wf = fw.open_window();
  if (!wf) { core::error("could not open a window (OpenGL)"); return false; }
  // Panda's own default display region (for the 3D render) is unused: views make their own.
  GraphicsOutput *w = wf->get_graphics_output();
  for (int i = 0; i < w->get_num_display_regions(); i++) w->get_display_region(i)->set_active(false);
  w->set_clear_color_active(true); w->set_clear_color(LColor(0.09f, 0.1f, 0.12f, 1));
  setupImGui();
  return true;
}

void App::setupImGui() {
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_NavEnableKeyboard;
  io.BackendPlatformName = "panda3d";
  GraphicsOutput *w = wf->get_graphics_output();
  if (!offscreen) {
    // generic button events -> ImGui (no per-button events, no modifier prefixes)
    NodePath mouse = wf->get_mouse();
    PT(ButtonThrower) bt = new ButtonThrower("imgui_buttons");
    bt->set_button_down_event("im-down"); bt->set_button_up_event("im-up"); bt->set_keystroke_event("im-key");
    // throw_buttons_active(true) with an empty list would swallow EVERY button (the 2e244e165 input bug);
    // we want all buttons, but only as the generic im-down/im-up events, not one event per button name.
    bt->set_throw_buttons_active(false); bt->set_specific_flag(false); bt->set_modifier_buttons(ModifierButtons());
    if (!mouse.is_empty()) mouse.attach_new_node(bt);
    else {   // enable_keyboard creates the data graph mouse node
      wf->enable_keyboard(); wf->get_mouse().attach_new_node(bt);
    }
    EventHandler &eh = fw.get_event_handler();
    eh.add_hook("im-down", onDown, nullptr); eh.add_hook("im-up", onUp, nullptr); eh.add_hook("im-key", onKey, nullptr);
  }
  PT(DisplayRegion) dr = w->make_display_region();
  dr->set_sort(1000); dr->set_clear_color_active(false); dr->set_clear_depth_active(false);
  dr->set_draw_callback(new ImGuiDraw());
}

GraphicsOutput *App::win() const { return wf->get_graphics_output(); }
int App::width() const { return win()->get_x_size(); }
int App::height() const { return win()->get_y_size(); }

bool App::step(const std::function<void()> &frame) {
  double t0 = core::nowMs();
  GraphicsOutput *w = win();
  ImGuiIO &io = ImGui::GetIO();
  io.DisplaySize = ImVec2((float)w->get_x_size(), (float)w->get_y_size());
  static double last = core::nowMs(); double now = core::nowMs();
  io.DeltaTime = std::max(1e-4, (now - last) / 1000.0); last = now;
  if (!offscreen && !injectInput && w->is_of_type(GraphicsWindow::get_class_type())) {
    auto *gw = DCAST(GraphicsWindow, w);
    if (gw->get_num_input_devices() > 0) {
      PointerData md = gw->get_pointer(0);
      g_input.inWindow = md.get_in_window(); g_input.px = (float)md.get_x(); g_input.py = (float)md.get_y();
      if (md.get_in_window()) io.AddMousePosEvent((float)md.get_x(), (float)md.get_y());
      else io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    }
    // Only report focus *changes*: ImGui clears all mouse/key state on every AddFocusEvent(false).
    bool fg = gw->get_properties().has_foreground() ? gw->get_properties().get_foreground() : true;
    static int lastFg = -1;
    if ((int)fg != lastFg) { io.AddFocusEvent(fg); lastFg = fg; if (g_inLog) fprintf(stderr, "[input] focus %d\n", (int)fg); }
    g_input.focused = fg;
  }
  if (!g_glInit) {   // the GL backend is created inside Panda's GL context on the first draw
    fw.do_frame(Thread::get_current_thread());
    if (!g_glInit) return true;
  }
  ImGui_ImplOpenGL3_NewFrame();
  ImGui::NewFrame();
  ImGuizmo::BeginFrame();
  frame();
  ImGui::Render();
  Thread *th = Thread::get_current_thread();
  bool alive = fw.do_frame(th);
  frameMs = core::nowMs() - t0;
  if (!offscreen && w->is_of_type(GraphicsWindow::get_class_type()) && DCAST(GraphicsWindow, w)->is_closed()) return false;
  return alive;
}

void App::run(const std::function<void()> &frame, const std::function<bool()> &wantClose) {
  while (step(frame)) { if (wantClose && wantClose()) break; }
}

bool App::screenshot(const std::string &path) {
  PNMImage img;
  if (!win()->get_screenshot(img)) return false;
  return img.write(Filename::from_os_specific(path));
}

void App::close() {
  // GL objects die with the context; ImGui context is freed with the process.
  fw.close_framework();
}

}  // namespace platform
