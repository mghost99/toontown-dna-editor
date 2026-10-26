// Native window (Panda3D GraphicsWindow, OpenGL) + Dear ImGui bridge: input from Panda's button/keystroke events,
// ImGui drawn by a draw callback on a top display region in Panda's GL context.
#pragma once
#include "pandaFramework.h"
#include <functional>
#include <string>
#include <vector>

namespace platform {

struct InputStats { long events = 0; std::string last; float px = -1, py = -1; bool inWindow = false, focused = false; };
extern InputStats g_input;   // for the F1 input debug line

struct Options { int width = 1600, height = 960; bool offscreen = false; bool vsync = true; std::string title = "DNA Editor"; std::string icon; };

// Native folder picker (Windows IFileOpenDialog; zenity/kdialog on Linux). "" if cancelled/unavailable.
std::string pickFolder(const std::string &title, const std::string &start = "");
// Native open/save file dialog with filters {label, "*.a;*.b"}. start: folder or file. "" if cancelled.
struct FileFilter { const char *label, *pattern; };
extern const std::vector<FileFilter> kDnaFilters;   // DNA (*.xml;*.dna;*.pdna), XML, DNA source, compiled DNA, all
std::string pickFile(const std::string &title, const std::string &start, bool save, const std::vector<FileFilter> &filters);

class App {
  float uiScale_ = 1.0f;
 public:
  bool open(int argc, char **argv, const Options &o);
  float uiScale() const { return uiScale_; }   // display zoom (1.25 at 125% Windows scaling)
  // Runs until closed. 'frame' builds the UI each frame (between ImGui::NewFrame and ImGui::Render).
  void run(const std::function<void()> &frame, const std::function<bool()> &wantClose = nullptr);
  bool step(const std::function<void()> &frame);   // one frame; false when the window closed
  void close();
  bool screenshot(const std::string &path);
  GraphicsOutput *win() const;
  int width() const; int height() const;
  NodePath render2d();   // not used for UI; kept for overlays
  PandaFramework fw;
  WindowFramework *wf = nullptr;
  bool offscreen = false;
  bool injectInput = false;   // self-test drives ImGui input itself; ignore the real pointer
  double frameMs = 0;
 private:
  void setupImGui();
};

}  // namespace platform
