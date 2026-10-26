// In-app log (shown in the Log panel) + scoped timers. Thread-safe append.
#pragma once
#include <chrono>
#include <string>
#include <vector>

namespace core {
enum class Level { Info, Warn, Error, Perf };
struct LogLine { Level level; std::string text; double t; };
void log(Level l, const std::string &msg);
inline void info(const std::string &m) { log(Level::Info, m); }
inline void warn(const std::string &m) { log(Level::Warn, m); }
inline void error(const std::string &m) { log(Level::Error, m); }
std::vector<LogLine> snapshot(size_t *version = nullptr);
size_t logVersion();
void clearLog();
double nowMs();

// Records "label: N ms" as a Perf line when it goes out of scope; also kept as last value per label.
class ScopedTimer {
 public:
  explicit ScopedTimer(std::string label, bool logIt = true);
  ~ScopedTimer();
  double elapsed() const;
 private:
  std::string label_; double t0_; bool log_;
};
double lastTiming(const std::string &label);
void setTiming(const std::string &label, double ms);
std::vector<std::pair<std::string, double>> timings();
}  // namespace core
