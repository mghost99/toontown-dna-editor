#include "log.h"
#include <cstdio>
#include <map>
#include <mutex>

namespace core {
static std::mutex g_mu;
static std::vector<LogLine> g_lines;
static size_t g_ver = 0;
static std::map<std::string, double> g_tim;
static auto g_t0 = std::chrono::steady_clock::now();

double nowMs() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - g_t0).count(); }
void log(Level l, const std::string &m) {
  std::lock_guard<std::mutex> g(g_mu);
  g_lines.push_back({l, m, nowMs()}); g_ver++;
  if (g_lines.size() > 5000) g_lines.erase(g_lines.begin(), g_lines.begin() + 1000);
  static const char *tag[] = {"info", "WARN", "ERROR", "perf"};
  fprintf(stderr, "[%s] %s\n", tag[(int)l], m.c_str());
}
std::vector<LogLine> snapshot(size_t *v) { std::lock_guard<std::mutex> g(g_mu); if (v) *v = g_ver; return g_lines; }
size_t logVersion() { std::lock_guard<std::mutex> g(g_mu); return g_ver; }
void clearLog() { std::lock_guard<std::mutex> g(g_mu); g_lines.clear(); g_ver++; }
ScopedTimer::ScopedTimer(std::string l, bool li) : label_(std::move(l)), t0_(nowMs()), log_(li) {}
double ScopedTimer::elapsed() const { return nowMs() - t0_; }
ScopedTimer::~ScopedTimer() {
  double e = elapsed(); setTiming(label_, e);
  if (log_) { char b[64]; snprintf(b, sizeof b, ": %.1f ms", e); log(Level::Perf, label_ + b); }
}
void setTiming(const std::string &l, double ms) { std::lock_guard<std::mutex> g(g_mu); g_tim[l] = ms; }
double lastTiming(const std::string &l) { std::lock_guard<std::mutex> g(g_mu); auto i = g_tim.find(l); return i == g_tim.end() ? -1 : i->second; }
std::vector<std::pair<std::string, double>> timings() { std::lock_guard<std::mutex> g(g_mu); return {g_tim.begin(), g_tim.end()}; }
}  // namespace core
