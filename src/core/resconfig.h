// Where the game's resources/ folder is.  Order: --res arg > DNAED_RES env > config file > auto-detect.
// Config: %APPDATA%\DnaEditor\config.ini (Windows) or ~/.config/dna-editor/config.ini, keys "resources=", "last_file=", "reopen_last=" (1 by default).
#pragma once
#include <string>
#include <vector>

namespace core {
bool isResourcesDir(const std::string &p);                 // has phase_4/ or phase_8/
std::string configPath();
std::string readConfig(const std::string &key);                      // "" if missing
bool writeConfig(const std::string &key, const std::string &value);   // keeps other keys
std::string readConfigRes();
bool writeConfigRes(const std::string &dir);
std::vector<std::string> defaultResCandidates(const char *argv0);
std::string autoDetectRes(const char *argv0);
// returns the chosen dir ("" if none) and how it was found ("--res", "DNAED_RES", "config", "auto")
std::string resolveRes(const std::string &arg, const char *argv0, std::string *source);
}  // namespace core
