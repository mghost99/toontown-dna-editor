#pragma once
#include "nodePath.h"
#include "animControl.h"
#include <string>
#include <vector>

namespace assets {
// Retain controls for as long as the model is visible. External animation names
// are relative to the rig's directory; embedded animations and sequences work too.
std::vector<PT(AnimControl)> loopModel(NodePath model, const std::string &modelPath, const std::string &animation = "", std::string *resolvedAnimation = nullptr);
void stopAnimations(std::vector<PT(AnimControl)> &controls);
}
