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
  friend int main(int, char **);   // scripted offscreen tests
  Editor(platform::App &app, std::string resRoot);
  bool open(const std::string &relOrAbs);
  bool save(const std::string &outDir = "");
  bool saveAs(const std::string &path);   // format by extension (.xml / .dna / .pdna)
  void frame();                 // builds the whole UI for this frame
  // operations (also used by --selftest)
  void select(const std::vector<int> &ids);
  void moveSelectedBy(const LVector3 &d, bool commit);
  void yawSelectedBy(float deg, bool commit);            // heading delta, one undo step if commit
  void pitchSelectedBy(float deg, bool commit);          // pitch delta, one undo step if commit
  void scaleSelectedUniform(float factor, bool commit);  // multiplicative uniform scale, clamp > 0.01
  void setPreviewEffects(bool on);   // live lantern flash + halo + fog; never writes XML
  bool previewEffects() const { return ovPreviewEffects_; }
  void setAnimateBats(bool on);        // flap/bob preview; never writes XML
  bool animateBats() const { return ovAnimateBats_; }
  float yawStep() const { return snap.on ? snap.angle : 15.f; }   // world-space delta, one undo step if commit
  float wheelAngleStep() const { return snap.on ? snap.angle : 5.f; }  // Ctrl/Alt+wheel heading/pitch step
  bool snapActive() const;  // snap.on and Alt not held (Alt = temporary bypass)
  void beginEdit(); void endEdit(const char *label);
  void duplicateSelected(); void deleteSelected();
  // Copy/paste anywhere (Ctrl+C / Ctrl+V; Ctrl+Shift+V = paste at zone level, detached from the parent model).
  // The clipboard survives File > Open, so props can move between files in one session.
  void copySelected();
  int pasteClipboard(bool zoneLevel, const LPoint3 *at = nullptr);
  LPoint3 clipboardCenter() const;
  size_t clipboardSize() const { return clip_.size(); }
  // Appearance Override playground context: the whole host area, never selectable, never saved.
  void rebuildContext();
  void setContextArea(const std::string &rel);
  const std::vector<std::string> &contextAreas() const { return ctxAreas_; }
  bool openPreview(int catIndex);
  int placePreviewed(const LPoint3 *at = nullptr, const dna::Xform *full = nullptr);
  // Wall-mount surface snap (banners, swags, drapes, bows): flush on a flat face, sized between its edges/pillars.
  bool isWallMount(const std::string &code) const;
  bool wallSnap(const render::PickHit &h, dna::Xform &out, std::string *why = nullptr);
  // Appearance Override: rewrite parent_code (ATTACH-TO-MODEL) props as plain world-space props
  // with the same rendered world pos/hpr/scale. Saves twice (transforms, then attribute strip). Returns count.
  int convertAttachToWorld(std::string *log = nullptr);
  bool wallSnapOn = true; NodePath snapRoot;   // empty = whole scene
  NodePath hostProp(const std::string &code);   // doc item, attach-parent holder, or playground-context copy   // View menu toggle; Alt disables while dragging   // at: world point (else the view-centre ground hit)
  int placeCatalog(int ci, const LPoint3 *at = nullptr);
  void paletteDropTarget(render::View &v);
  int dropCatalog(int ci, render::View &v, float px, float py);   // drag-drop: ground/surface under the pixel
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
  bool invertCamera_ = true;   // grab-the-world default; View > Invert camera
  bool invertCameraY_ = true;  // pitch/pan Y (same default)
  float wheelSpeed = 1.5f;   // minimum wheel step (ft per notch); grows with distance to the cursor point
  float lastWheelBase_ = 0;  // step per notch used by the last wheel zoom (selftest)
  // 3D wheel zoom toward the cursor: step scales with distance to the surface under the cursor
  // (else height above ground). fast = Alt (x5), fine = Shift (x0.2, when nothing is selected).
  float wheelZoom(float px, float py, float wheel, bool fast, bool fine);
  bool focusCursor(float px, float py);   // F with nothing selected: frame the point under the cursor
 private:
  platform::App &app_;
  assets::Storage storage_;
  render::Lighting light_;
  NodePath previewRoot_, previewModel_;
  std::vector<PT(AnimControl)> previewAnimations_;
  std::string previewAnimation_;
  int previewIdx_ = -1; LVecBase3 previewSize_; LPoint3 previewMin_, previewMax_; NodePath dropGhost_; int dropGhostFrame_ = -1; int previewTris_ = 0; float pvYaw_ = 35, pvPitch_ = 20, pvZoom_ = 1;
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
  int lastView_ = 3;     // view last clicked (2 = 2D Map, 3 = 3D View); "Place in scene" uses its centre
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
  bool consumeSelectionWheel(const ImGuiIO &io);  // Shift/Ctrl(/Alt) wheel on selection; sets wheelUsed_
  void viewport3D(); void viewport2D(); void previewWindow(); void menuBar(); void shortcuts(); void statusBar();
  void visgroupsPanel(); void statsPanel(); void openDialog(); void resDialog(); void passwordDialog();
  // Appearance Override DNA (opt-in): panel only when the open file has appearance_override="1".
  void overridePanel();
  bool isAppearanceOverride() const;
  bool injectOverrideOp(const std::string &tag, const std::string &code,
                        const std::string &extraAttrs = "");
  bool setOverrideGate(const std::string &gate, const std::string &holidays);
  void applyContextOverride();   // preview hide/replace/recolor entries on the view-only playground (game parity)
  bool editOverrideText(const std::string &newText, const std::string &label);   // one undo step, re-previews
  bool replaceBaseWithCatalog(int ctxIdx, int ci);
  bool removeOverrideOpsFor(const std::string &code);
  std::vector<NodePath> ctxOvAdded_, ctxOvStashed_, ctxOvRecolored_;
  int ovBase_ = -1, ovSwaps_ = 0, ovHidden_ = 0, ovRecolors_ = 0;
  void rebuildNpcMarkers();   // view-only labels at npc_origin_N / party_person_* (names from NPCToons.py), never saved
  bool showNpcs_ = true; NodePath npcRoot_; int npcCount_ = 0; std::vector<std::string> npcLabels_, npcUnused_; bool showUnusedNpcs_ = false;
  bool newAppearanceOverride();
  // encrypted .pdna: modal password prompt (password kept in memory only, never logged/saved)
  bool showPw_ = false; std::string pwFile_, pwMsg_; char pwBuf_[256] = {0};
  bool showRes_ = false; char resBuf_[1024] = {0}; std::string resMsg_;
  bool overrideDirty_ = false;
  char ovGate_[32] = "holiday";
  char ovHolidays_[256] = "HALLOWEEN_PROPS,SPOOKY_PROPS";
  char ovCode_[128] = "";
  char ovRecolor_[32] = "1 1 1 1";
  char ovParentCode_[128] = "prop_gazebo";  // ATTACH-TO-MODEL parent DNA code
  char ovParentNode_[128] = "";             // optional sub-node under parent
  // Effect / lighting (AODNA <effect>); lightTuner gazebo field names
  bool ovFxLoaded_ = false;
  float ovFxMistOpacity_ = 0.14f, ovFxMistDensity_ = 0.35f, ovFxMistHeight_ = 0.55f;
  float ovFxMistSpeed_ = 0.40f, ovFxMistHue_ = 0.28f, ovFxMistRadius_ = 8.0f;
  float ovFxLanternSize_ = 1.45f, ovFxLanternIntensity_ = 0.38f, ovFxFlashSpeed_ = 1.0f;
  // Jack-o'-lantern face-only candle glow (game: HalloweenFaceGlow)
  float ovFxFaceGlow_ = 1.6f, ovFxFlickerAmount_ = 0.35f, ovFxFlickerSpeed_ = 1.0f;
  bool ovPreviewEffects_ = true;           // Preview effects ON by default (live only, no XML)
  bool ovAnimateBats_ = false;             // Bat flap/bob OFF by default (placement-friendly)
  NodePath fxPreviewRoot_;                 // labels + fog + halos (not saved)
  PT(Texture) fxHaloTex_, fxCloudTex_, fxBatUpTex_, fxBatDownTex_, fxFaceMaskTex_;
  bool fxBatsSwapped_ = false;            // a wing texture override is on some bat
  NodePath faceLayerFor(NodePath np, bool &decal);  // face_glow child or cached mask decal
  bool fxPreviewBuilt_ = false;
  void loadOverrideEffectsFromDoc();
  bool applyOverrideEffects();
  void updateEffectsPreview();             // flash + halo + fog; no XML
  void clearEffectsPreview();
  void updateBatsPreview();                // flap + bob; restores rest pose when off
  PT(Texture) ensureHaloTex();
  PT(Texture) ensureCloudTex();
  void drawSelection(render::View &v, bool topDown);
  bool handleDrag(render::View &v, bool topDown, bool hovered);
  void pushXformFromNode(int id, bool fromGizmo = false);
  LPoint3 placementPoint();
  float snapv(float v) const;
  struct ClipItem { std::string xml, kind, name, code, visgroup; dna::Xform xf, origXf; LMatrix4 world; };
  std::vector<ClipItem> clip_;
  bool ctxShow_ = true; std::string ctxRel_; std::vector<std::string> ctxAreas_, ctxHosts_;
  render::Scene ctxScene_; dna::Document ctxDoc_; assets::Storage ctxStorage_; bool ctxBuilt_ = false;
  void scanContextAreas();
  void detachContext();
  void attachContext();
};
