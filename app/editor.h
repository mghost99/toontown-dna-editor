// The editor controller: owns document, storage, scene, views, history and wires the UI panels to them.
#pragma once
#include "imgui.h"
#include "assets/storage.h"
#include "commands/commands.h"
#include "dna/dna_document.h"
#include "platform/platform.h"
#include "render/gizmo.h"
#include "render/scene.h"
#include "render/views.h"
#include "render/walker.h"
#include "ui/ui.h"
#include <memory>

class Editor {
 public:
  Editor(platform::App &app, std::string resRoot);
  bool open(const std::string &relOrAbs);
  bool save(const std::string &outDir = "");
  bool saveAs(const std::string &path);   // format by extension (.xml / .dna / .pdna)
  void frame();                 // builds the whole UI for this frame
  // operations (also used by --selftest)
  void select(const std::vector<int> &ids);
  void moveSelectedBy(const LVector3 &d, bool commit);
  void yawSelectedBy(float deg, bool commit);            // heading delta, one undo step if commit
  float yawStep() const { return snap.on ? snap.angle : 15.f; }   // world-space delta, one undo step if commit
  void beginEdit(); void endEdit(const char *label);
  void duplicateSelected(); void deleteSelected();
  bool openPreview(int catIndex);
  int placePreviewed();
  void setLight(render::LightMode m);
  void frameAll(); void frameSelection();
  int  pickAt3D(float px, float py);
  commands::History history;
  dna::Document doc;
  std::vector<int> selection;
  std::vector<dna::CatalogEntry> catalog;
  std::unique_ptr<render::View> v3, v2, pv;
  render::Scene scene;
  render::Walker walker;
  bool toggleWalker();
  size_t previewAnimationCount() const { return previewAnimations_.size(); }
  render::GizmoOp gizmoOp = render::GizmoOp::Translate;
  render::GizmoSnap snap;
  bool quit = false;
  std::string resRoot;
  double lastFrameMs = 0;
  // test hooks
  bool forceViews = false;
  float wheelSpeed = 1.5f;
 private:
  platform::App &app_;
  assets::Storage storage_;
  render::Lighting light_;
  NodePath previewRoot_, previewModel_;
  std::vector<PT(AnimControl)> previewAnimations_;
  std::string previewAnimation_;
  int previewIdx_ = -1; LVecBase3 previewSize_; int previewTris_ = 0; float pvYaw_ = 35, pvPitch_ = 20, pvZoom_ = 1;
  ui::PaletteState pal_; ui::OutlinerState out_;
  bool showInputDebug_ = getenv("DNAED_INPUT_DEBUG") != nullptr; std::string hoveredView_ = "none";
  bool logAuto_ = true, showOpen_ = false, showHelp_ = false, layoutDone_ = false, showDemo_ = false;
  bool show3D_ = true, showMap_ = true, showPalette_ = true, showVisgroups_ = true;
  bool showInspector_ = true, showObjects_ = true, showPreview_ = true, showLog_ = true, showTimings_ = true;
  std::vector<dna::Xform> editBefore_; std::vector<int> editIds_; bool editing_ = false;
  // drag state
  bool dragging_ = false, dragArmed_ = false; int dragView_ = 0; float dragZ_ = 0; LPoint3 dragLast_; ImVec2 pressPos_;
  bool orbiting_ = false;
  LPoint3 walkerCameraTarget_;
  float walkerCameraDist_ = 0, walkerCameraYaw_ = 0, walkerCameraPitch_ = 0;
  int pointerView_ = 0;   // 2 or 3 while a mouse button is held; the other view ignores that drag
  bool rotDrag_ = false; float rotAccum_ = 0; std::vector<float> rotStartH_;   // Shift+left-drag yaw
  bool wheelUsed_ = false;   // wheel consumed by a drag (yaw) this frame
  char openFilter_[128] = "";
  std::vector<std::string> files_;
  std::map<std::string, bool> vgVisible_;
  std::set<std::string> loadedCodes_;
  struct Box { LPoint3 mn, mx; bool ok = false; };
  std::map<int, Box> boxes_;
  const Box &boxOf(int i);
  bool rayBox(int i, const LPoint3 &o, const LVector3 &d, float &tEnter);
  int pickItem(render::View &v, float px, float py, LPoint3 &hitPt);   // whole-object pick (selected box first)
  render::GizmoOp prevOp_ = render::GizmoOp::None;
  void rebuildScene();
  void syncItems(const std::vector<int> &ids);
  void dockLayout(unsigned id);
  void viewport3D(); void viewport2D(); void previewWindow(); void menuBar(); void shortcuts(); void statusBar();
  void visgroupsPanel(); void statsPanel(); void openDialog(); void resDialog(); void passwordDialog();
  // encrypted .pdna: modal password prompt (password kept in memory only, never logged/saved)
  bool showPw_ = false; std::string pwFile_, pwMsg_; char pwBuf_[256] = {0};
  bool showRes_ = false; char resBuf_[1024] = {0}; std::string resMsg_;
  void drawSelection(render::View &v, bool topDown);
  bool handleDrag(render::View &v, bool topDown, bool hovered);
  void pushXformFromNode(int id, bool fromGizmo = false);
  LPoint3 placementPoint();
  float snapv(float v) const;
};
