#include "ui.h"
#include "core/log.h"
#include "imgui.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>

namespace ui {

void applyTheme(float scale) {
  ImGuiIO &io = ImGui::GetIO();
  const char *fonts[] = {
#ifdef _WIN32
                         "C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/arial.ttf",
#endif
                         "/usr/share/fonts/inter/Inter-Regular.ttf", "/usr/share/fonts/TTF/Inter-Regular.ttf",
                         "/usr/share/fonts/noto/NotoSans-Regular.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf",
                         "/usr/share/fonts/liberation/LiberationSans-Regular.ttf"};
  for (auto f : fonts) if (std::filesystem::exists(f)) { io.Fonts->AddFontFromFileTTF(f, 16.0f * scale); break; }
  ImGuiStyle &s = ImGui::GetStyle();
  ImGui::StyleColorsDark(&s);
  s.WindowRounding = 6; s.FrameRounding = 5; s.GrabRounding = 5; s.TabRounding = 5; s.PopupRounding = 6;
  s.ChildRounding = 5; s.ScrollbarRounding = 8; s.WindowBorderSize = 0; s.FrameBorderSize = 0;
  s.WindowPadding = ImVec2(10, 10); s.FramePadding = ImVec2(8, 5); s.ItemSpacing = ImVec2(8, 6);
  s.ScrollbarSize = 12; s.TabBarBorderSize = 0; s.DockingSeparatorSize = 2;
  ImVec4 *c = s.Colors;
  ImVec4 bg(0.105f, 0.11f, 0.13f, 1), bg2(0.135f, 0.14f, 0.165f, 1), bg3(0.18f, 0.19f, 0.22f, 1);
  ImVec4 acc(0.36f, 0.55f, 1.0f, 1), accD(0.26f, 0.42f, 0.85f, 1);
  c[ImGuiCol_WindowBg] = bg; c[ImGuiCol_ChildBg] = bg; c[ImGuiCol_PopupBg] = ImVec4(0.12f, 0.125f, 0.15f, 0.98f);
  c[ImGuiCol_MenuBarBg] = ImVec4(0.085f, 0.09f, 0.105f, 1); c[ImGuiCol_TitleBg] = ImVec4(0.085f, 0.09f, 0.105f, 1);
  c[ImGuiCol_TitleBgActive] = ImVec4(0.085f, 0.09f, 0.105f, 1);
  c[ImGuiCol_FrameBg] = bg2; c[ImGuiCol_FrameBgHovered] = bg3; c[ImGuiCol_FrameBgActive] = ImVec4(0.22f, 0.24f, 0.29f, 1);
  c[ImGuiCol_Button] = bg3; c[ImGuiCol_ButtonHovered] = ImVec4(0.25f, 0.28f, 0.34f, 1); c[ImGuiCol_ButtonActive] = accD;
  c[ImGuiCol_Header] = ImVec4(0.2f, 0.27f, 0.42f, 1); c[ImGuiCol_HeaderHovered] = ImVec4(0.24f, 0.33f, 0.52f, 1);
  c[ImGuiCol_HeaderActive] = accD; c[ImGuiCol_CheckMark] = acc; c[ImGuiCol_SliderGrab] = acc; c[ImGuiCol_SliderGrabActive] = accD;
  c[ImGuiCol_Tab] = bg; c[ImGuiCol_TabHovered] = bg3; c[ImGuiCol_TabSelected] = bg2; c[ImGuiCol_TabDimmed] = bg;
  c[ImGuiCol_TabDimmedSelected] = bg2; c[ImGuiCol_TabSelectedOverline] = acc;
  c[ImGuiCol_DockingPreview] = ImVec4(0.36f, 0.55f, 1.0f, 0.5f); c[ImGuiCol_Separator] = ImVec4(0.2f, 0.21f, 0.25f, 1);
  c[ImGuiCol_Border] = ImVec4(0.2f, 0.21f, 0.25f, 1); c[ImGuiCol_Text] = ImVec4(0.9f, 0.91f, 0.94f, 1);
  c[ImGuiCol_TextDisabled] = ImVec4(0.5f, 0.53f, 0.6f, 1);
  s.ScaleAllSizes(scale);
}

static std::string lower(std::string s) { for (auto &c : s) c = (char)std::tolower((unsigned char)c); return s; }

int palettePanel(const std::vector<dna::CatalogEntry> &cat, PaletteState &st, const std::set<std::string> &loaded) {
  int clicked = -1;
  ImGui::SetNextItemWidth(-1);
  ImGui::InputTextWithHint("##pf", "Search props (name, model, storage)...", st.filter, sizeof st.filter);
  const char *roots[] = {"All", "In this file's storage", "prop", "anim_prop", "interactive_prop", "holiday_prop"};
  ImGui::SetNextItemWidth(-1);
  ImGui::Combo("##root", &st.rootFilter, roots, IM_ARRAYSIZE(roots));
  if (st.lastFilter != st.filter || st.lastRoot != st.rootFilter) {   // filter only when it changes
    st.lastFilter = st.filter; st.lastRoot = st.rootFilter; st.visible.clear();
    std::string f = lower(st.filter);
    for (int i = 0; i < (int)cat.size(); i++) {
      auto &e = cat[i];
      if (st.rootFilter == 1 && !loaded.count(e.code)) continue;
      if (st.rootFilter >= 2 && e.root != roots[st.rootFilter]) continue;
      if (!f.empty() && lower(e.code + " " + e.model + " " + e.storageRel).find(f) == std::string::npos) continue;
      st.visible.push_back(i);
    }
  }
  ImGui::TextDisabled("%d of %d props", (int)st.visible.size(), (int)cat.size());
  ImGui::BeginChild("plist", ImVec2(0, 0), ImGuiChildFlags_None);
  ImGuiListClipper clip; clip.Begin((int)st.visible.size());
  while (clip.Step())
    for (int r = clip.DisplayStart; r < clip.DisplayEnd; r++) {
      int i = st.visible[r]; auto &e = cat[i];
      ImGui::PushID(i);
      bool here = loaded.count(e.code) > 0;
      if (ImGui::Selectable("##s", st.selected == i, 0, ImVec2(0, 0))) { st.selected = i; clicked = i; }
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\nmodel: %s\nstorage: %s", e.code.c_str(), e.model.c_str(), e.storageRel.c_str());
      ImGui::SameLine(0, 0);
      ImGui::TextColored(here ? ImVec4(0.55f, 0.85f, 0.6f, 1) : ImVec4(0.45f, 0.48f, 0.55f, 1), here ? "\xE2\x97\x8F " : "\xE2\x97\x8B ");
      ImGui::SameLine(0, 2); ImGui::TextUnformatted(e.code.c_str());
      ImGui::PopID();
    }
  ImGui::EndChild();
  return clicked;
}

bool outlinerPanel(const dna::Document &doc, std::vector<int> &sel, OutlinerState &st, bool *focus) {
  bool changed = false;
  ImGui::SetNextItemWidth(-1);
  ImGui::InputTextWithHint("##of", "Filter objects...", st.filter, sizeof st.filter);
  std::string f = lower(st.filter);
  ImGui::BeginChild("olist");
  if (ImGui::BeginTable("ot", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn("Name"); ImGui::TableSetupColumn("Kind"); ImGui::TableSetupColumn("Visgroup");
    ImGui::TableSetupScrollFreeze(0, 1); ImGui::TableHeadersRow();
    static std::vector<int> rows; rows.clear();
    for (int i = 0; i < (int)doc.items.size(); i++) {
      auto &it = doc.items[i]; if (it.deleted) continue;
      if (!st.visgroup.empty() && it.visgroup != st.visgroup) continue;
      if (!f.empty() && lower(it.name + " " + it.code + " " + it.kind).find(f) == std::string::npos) continue;
      rows.push_back(i);
    }
    ImGuiListClipper clip; clip.Begin((int)rows.size());
    while (clip.Step())
      for (int r = clip.DisplayStart; r < clip.DisplayEnd; r++) {
        int i = rows[r]; auto &it = doc.items[i];
        ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::PushID(i);
        bool isSel = std::find(sel.begin(), sel.end(), i) != sel.end();
        std::string label = (it.name.empty() ? it.code : it.name) + (it.origId < 0 ? "  (new)" : "");
        if (ImGui::Selectable(label.c_str(), isSel, ImGuiSelectableFlags_SpanAllColumns)) {
          if (ImGui::GetIO().KeyCtrl) { if (isSel) sel.erase(std::find(sel.begin(), sel.end(), i)); else sel.push_back(i); }
          else sel = {i};
          changed = true;
        }
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0) && focus) *focus = true;
        ImGui::TableNextColumn(); ImGui::TextDisabled("%s", it.kind.c_str());
        ImGui::TableNextColumn(); ImGui::TextDisabled("%s", it.visgroup.c_str());
        ImGui::PopID();
      }
    ImGui::EndTable();
  }
  ImGui::EndChild();
  return changed;
}

int inspectorPanel(dna::Item *it, int index, dna::Xform &x) {
  if (!it) { ImGui::TextDisabled("Nothing selected.\nClick an object in the 3D view, 2D map or Objects list."); return 0; }
  ImGui::Text("%s", it->name.empty() ? it->code.c_str() : it->name.c_str());
  ImGui::TextDisabled("%s  |  code %s  |  #%d%s", it->kind.c_str(), it->code.c_str(), index, it->nested ? "  |  nested" : "");
  if (!it->visgroup.empty()) ImGui::TextDisabled("visgroup %s", it->visgroup.c_str());
  ImGui::Separator();
  int r = 0;
  auto row = [&](const char *lbl, double *v, float speed) {
    float f[3] = {(float)v[0], (float)v[1], (float)v[2]};
    ImGui::SetNextItemWidth(-60);
    if (ImGui::DragFloat3(lbl, f, speed, 0, 0, "%.2f")) { v[0] = f[0]; v[1] = f[1]; v[2] = f[2]; r = std::max(r, 1); }
    if (ImGui::IsItemDeactivatedAfterEdit()) r = 2;
  };
  double p[3] = {x.x, x.y, x.z}, h[3] = {x.h, x.p, x.r}, s[3] = {x.sx, x.sy, x.sz};
  row("Pos", p, 0.1f); row("H P R", h, 0.5f); row("Scale", s, 0.01f);
  x.x = p[0]; x.y = p[1]; x.z = p[2]; x.h = h[0]; x.p = h[1]; x.r = h[2]; x.sx = s[0]; x.sy = s[1]; x.sz = s[2];
  if (it->kind == "flat_building") ImGui::TextDisabled("width %.1f", it->width);
  return r;
}

void logPanel(bool *autoScroll) {
  static int filter = 0;
  if (ImGui::Button("Clear")) core::clearLog();
  ImGui::SameLine(); ImGui::Checkbox("Auto-scroll", autoScroll);
  ImGui::SameLine(); ImGui::SetNextItemWidth(140);
  const char *f[] = {"All", "Warnings+", "Errors", "Timings"}; ImGui::Combo("##lf", &filter, f, 4);
  ImGui::BeginChild("logc", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
  static std::vector<core::LogLine> lines; static size_t ver = (size_t)-1;
  if (core::logVersion() != ver) lines = core::snapshot(&ver);
  static std::vector<int> rows; rows.clear();
  for (int i = 0; i < (int)lines.size(); i++) {
    auto l = lines[i].level;
    if (filter == 1 && !(l == core::Level::Warn || l == core::Level::Error)) continue;
    if (filter == 2 && l != core::Level::Error) continue;
    if (filter == 3 && l != core::Level::Perf) continue;
    rows.push_back(i);
  }
  ImGuiListClipper clip; clip.Begin((int)rows.size());
  while (clip.Step())
    for (int r = clip.DisplayStart; r < clip.DisplayEnd; r++) {
      auto &l = lines[rows[r]];
      ImVec4 col = l.level == core::Level::Error ? ImVec4(1, 0.45f, 0.45f, 1) : l.level == core::Level::Warn ? ImVec4(1, 0.8f, 0.35f, 1)
                   : l.level == core::Level::Perf ? ImVec4(0.55f, 0.75f, 1, 1) : ImVec4(0.8f, 0.82f, 0.86f, 1);
      ImGui::TextColored(ImVec4(0.45f, 0.48f, 0.55f, 1), "%8.2fs", l.t / 1000.0); ImGui::SameLine();
      ImGui::TextColored(col, "%s", l.text.c_str());
    }
  if (*autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 40) ImGui::SetScrollHereY(1.0f);
  ImGui::EndChild();
}

void helpPanel() {
  ImGui::TextUnformatted(
      "Mouse (3D view)\n"
      "  Left click        select (Ctrl = add to selection)\n"
      "  Left drag object  free drag at its current height\n"
      "  Shift+Left drag   rotate (yaw) the selection; wheel while dragging = yaw one step\n"
      "  Right drag        orbit      Middle drag / Shift+Right  pan\n"
      "  Wheel             zoom       F  frame selection\n"
      "Mouse (2D map)\n"
      "  Left drag object  move      Right/Middle drag  pan   Wheel  zoom\n"
      "Keys\n"
      "  W / E / R         translate / rotate (H/P/R rings; E again = back) / scale\n"
      "  [ / ]             yaw -/+ one step (15 deg, or the snap angle); Shift = 90 deg\n"
      "  Q                 gizmo off (drag only)\n"
      "  X                 toggle snapping       L  cycle lighting (Game / Lit / Cog mood)\n"
      "  Ctrl+Z / Ctrl+Y   undo / redo (Ctrl+Shift+Z too)\n"
      "  Ctrl+D            duplicate      Delete  delete\n"
      "  Ctrl+S            save (writes .bak first)   Ctrl+O  open\n"
      "  F1                input debug line   F2  this help\n"
      "  Esc               deselect\n");
}

}  // namespace ui
