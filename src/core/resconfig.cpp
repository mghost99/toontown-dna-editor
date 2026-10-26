#include "resconfig.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
namespace core {

static std::string env(const char *k) { const char *v = getenv(k); return v ? v : ""; }

bool isResourcesDir(const std::string &p) {
  std::error_code ec;
  return !p.empty() && (fs::is_directory(fs::path(p) / "phase_4", ec) || fs::is_directory(fs::path(p) / "phase_8", ec));
}

std::string configPath() {
#ifdef _WIN32
  std::string base = env("APPDATA"); if (base.empty()) base = env("USERPROFILE");
  return (fs::path(base) / "DnaEditor" / "config.ini").string();
#else
  std::string base = env("XDG_CONFIG_HOME"); if (base.empty()) base = env("HOME") + "/.config";
  return (fs::path(base) / "dna-editor" / "config.ini").string();
#endif
}

std::string readConfig(const std::string &key) {
  std::ifstream f(configPath()); std::string line, k = key + "=";
  while (std::getline(f, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.rfind(k, 0) == 0) return line.substr(k.size());
  }
  return "";
}

bool writeConfig(const std::string &key, const std::string &value) {
  fs::path p = configPath(); std::error_code ec; fs::create_directories(p.parent_path(), ec);
  std::vector<std::string> keep; bool done = false; std::string k = key + "=";
  { std::ifstream f(p); std::string l;
    while (std::getline(f, l)) {
      if (!l.empty() && l.back() == '\r') l.pop_back();
      if (l.empty() || l.rfind("[", 0) == 0) continue;
      if (l.rfind(k, 0) == 0) { if (!done) keep.push_back(k + value); done = true; } else keep.push_back(l);
    } }
  if (!done) keep.push_back(k + value);
  std::ofstream o(p, std::ios::trunc); if (!o) return false;
  o << "[editor]\n"; for (auto &l : keep) o << l << "\n";
  return (bool)o;
}

std::string readConfigRes() { return readConfig("resources"); }
bool writeConfigRes(const std::string &dir) { return writeConfig("resources", dir); }

std::vector<std::string> defaultResCandidates(const char *argv0) {
  std::vector<std::string> c;
  c.push_back((fs::current_path() / "resources").string());
  std::error_code ec; fs::path p = fs::weakly_canonical(fs::path(argv0), ec).parent_path();
  for (int i = 0; i < 6 && !p.empty(); i++, p = p.parent_path()) c.push_back((p / "resources").string());
#ifdef _WIN32
  std::string home = env("USERPROFILE");
#else
  std::string home = env("HOME");
#endif
  return c;
}

std::string autoDetectRes(const char *argv0) {
  for (auto &x : defaultResCandidates(argv0)) if (isResourcesDir(x)) return x;
  return "";
}

std::string resolveRes(const std::string &arg, const char *argv0, std::string *source) {
  auto pick = [&](const std::string &d, const char *s) { if (source) *source = s; return d; };
  if (!arg.empty()) return pick(arg, "--res");
  std::string e = env("DNAED_RES"); if (isResourcesDir(e)) return pick(e, "DNAED_RES");
  std::string c = readConfigRes(); if (isResourcesDir(c)) return pick(c, "config");
  return pick(autoDetectRes(argv0), "auto");
}
}  // namespace core
