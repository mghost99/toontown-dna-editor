// ImGui panels (no engine access): theme, palette list, outliner, inspector, log, timing HUD.
#pragma once
#include "dna/dna_document.h"
#include <set>
#include <string>
#include <vector>

namespace ui {

void applyTheme(float scale = 1.0f);   // modern dark theme + Noto Sans / Inter if present

struct PaletteState { char filter[128] = ""; int rootFilter = 0; int selected = -1; std::vector<int> visible; std::string lastFilter = "\x01"; int lastRoot = -1; };
// Returns the catalog index the user clicked this frame (-1 none).
int palettePanel(const std::vector<dna::CatalogEntry> &cat, PaletteState &st, const std::set<std::string> &loadedCodes);

struct OutlinerState { char filter[128] = ""; std::string visgroup; };
// Click selects (ctrl adds). Returns true if selection changed.
bool outlinerPanel(const dna::Document &doc, std::vector<int> &selection, OutlinerState &st, bool *focusRequest);

struct InspectorField { std::string id; float x = 0, y = 0; };
extern std::vector<InspectorField> inspectorFields; // widget positions for injected-input tests
struct SignEdit { const dna::XmlNode *baseline = nullptr, *node = nullptr; std::string text, key, value; bool commit = false; };
// Edits xf of the single selected item. Returns 1 while editing (live), 2 when an edit finished (commit), 0 none.
// Sign lines are one string here; the file still stores one letters entry per character.
int inspectorPanel(dna::Item *it, int index, dna::Xform &editXf, const dna::Document *doc, SignEdit *sign = nullptr);

void logPanel(bool *autoScroll);
void helpPanel();

}  // namespace ui
