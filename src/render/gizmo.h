// ImGuizmo bridge for Panda3D cameras (Z-up, row-vector matrices) -- W/E/R translate/rotate/scale.
#pragma once
#include "views.h"
#include "luse.h"

namespace render {
enum class GizmoOp { None, Translate, Rotate, Scale };
struct GizmoSnap { bool on = true; float move = 1.0f, angle = 15.0f, scale = 0.1f; };
// Draws/handles the gizmo for 'node' inside the view rect. Returns true while the user manipulates it; then
// 'node' has its new transform (relative to its parent).
bool gizmo(const View &v, NodePath node, GizmoOp op, const GizmoSnap &snap, bool *hovered, bool *changed = nullptr, const LPoint3 &pivotLocal = LPoint3(0));
}
