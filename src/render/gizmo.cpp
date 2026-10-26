#include "gizmo.h"
#include "imgui.h"
#include "ImGuizmo.h"
#include "compose_matrix.h"

namespace render {
bool gizmo(const View &v, NodePath node, GizmoOp op, const GizmoSnap &snap, bool *hovered, bool *changedOut, const LPoint3 &pivotLocal) {
  if (changedOut) *changedOut = false;
  if (hovered) *hovered = false;
  if (op == GizmoOp::None || node.is_empty()) return false;
  NodePath top = node.get_top();
  LMatrix4f C = LMatrix4f::convert_mat(CS_zup_right, CS_yup_right), Ci = LMatrix4f::convert_mat(CS_yup_right, CS_zup_right);
  LMatrix4f camInv; camInv.invert_from(LCAST(float, v.cam.get_mat(top)));
  LMatrix4f view = camInv * C;
  LMatrix4f proj = Ci * LCAST(float, v.lens->get_projection_mat());
  // draw/handle the gizmo at the object's bounds centre (DNA props often have their pivot far from the geometry)
  LMatrix4f piv = LMatrix4f::translate_mat(LCAST(float, LVecBase3(pivotLocal)));
  LMatrix4f pivInv = LMatrix4f::translate_mat(-LCAST(float, LVecBase3(pivotLocal)));
  LMatrix4f m = piv * LCAST(float, node.get_mat(top));
  ImGuizmo::SetOrthographic(v.ortho);
  ImGuizmo::SetGizmoSizeClipSpace(0.18f);
  ImGuizmo::AllowAxisFlip(false);
  ImGuizmo::SetDrawlist(ImGui::GetWindowDrawList());
  Rect r = v.rect(); ImGuizmo::SetRect(r.x, r.y, r.w, r.h);
  ImGuizmo::OPERATION o = op == GizmoOp::Translate ? ImGuizmo::TRANSLATE : op == GizmoOp::Rotate ? (ImGuizmo::OPERATION)(ImGuizmo::ROTATE_X | ImGuizmo::ROTATE_Y | ImGuizmo::ROTATE_Z) : ImGuizmo::SCALE;
  float sn[3]; float s = op == GizmoOp::Translate ? snap.move : op == GizmoOp::Rotate ? snap.angle : snap.scale; sn[0] = sn[1] = sn[2] = s;
  ImGuizmo::MODE mode = op == GizmoOp::Scale ? ImGuizmo::LOCAL : (op == GizmoOp::Rotate ? ImGuizmo::LOCAL : ImGuizmo::WORLD);
  bool changed = ImGuizmo::Manipulate(view.get_data(), proj.get_data(), o, mode, const_cast<float *>(m.get_data()), nullptr, snap.on ? sn : nullptr);
  if (hovered) *hovered = ImGuizmo::IsOver() || ImGuizmo::IsUsing();
  if (changedOut) *changedOut = changed;
  if (changed) {
    LMatrix4 parentInv; parentInv.invert_from(node.get_parent().get_mat(top));
    LMatrix4 local = LCAST(PN_stdfloat, pivInv * m) * parentInv;
    LVecBase3 sc, sh, hpr, pos;
    decompose_matrix(local, sc, sh, hpr, pos);
    node.set_pos_hpr_scale(pos, hpr, sc);
  }
  return ImGuizmo::IsUsing();
}
}
