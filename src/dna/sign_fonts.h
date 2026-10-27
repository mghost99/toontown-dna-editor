#pragma once
// Built-in font paths for sign baseline codes. These are used only when no loaded
// storage file declares the code with store_font; storage always wins.
#include <string>

namespace dna {

// Resource-relative font path for a conventional font code, or "" when the code is unknown.
inline std::string fallbackSignFontPath(const std::string &code) {
  if (code == "humanist") return "phase_3/fonts/Humanist.ttf";
  if (code == "mickey" || code == "sign" || code == "classic") return "phase_3/fonts/MickeyFont.bam";
  if (code == "fancy" || code == "TT_Comedy") return "phase_3/fonts/Comedy.ttf";
  if (code == "interface" || code == "interfaceFont" || code == "impact") return "phase_3/fonts/ImpressBT.ttf";
  if (code == "suit" || code == "cog") return "phase_3/fonts/vtRemingtonPortable.ttf";
  if (code == "minnie" || code == "TT_Minnie") return "phase_3/fonts/MinnieFont.ttf";
  auto us = code.find('_');   // hood fonts such as "DD_Portago" -> Portago.ttf
  if (us != std::string::npos) {
    std::string name = code.substr(us + 1);
    if (!name.empty() && name.find_first_of("/\\.") == std::string::npos) return "phase_3/fonts/" + name + ".ttf";
  }
  return "";
}

// Display name for the fallback face of a code.
inline std::string fallbackSignFontName(const std::string &code) {
  if (code.empty()) return "default";
  std::string p = fallbackSignFontPath(code);
  if (p.empty()) return "none (editor default font)";
  auto s = p.find_last_of('/'); auto e = p.find_last_of('.');
  return p.substr(s + 1, e == std::string::npos || e < s ? std::string::npos : e - s - 1);
}

}  // namespace dna
