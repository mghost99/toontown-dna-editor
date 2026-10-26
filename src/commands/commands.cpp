#include "commands.h"

namespace commands {
void History::pushApplied(std::unique_ptr<Command> c) {
  redo_.clear(); undo_.push_back(std::move(c));
  if (undo_.size() > 500) undo_.erase(undo_.begin());
}
void History::execute(dna::Document &d, std::unique_ptr<Command> c) {
  c->redo(d); auto t = c->touched(); pushApplied(std::move(c)); if (onChanged) onChanged(t);
}
bool History::undo(dna::Document &d) {
  if (undo_.empty()) return false;
  auto c = std::move(undo_.back()); undo_.pop_back(); c->undo(d); auto t = c->touched();
  redo_.push_back(std::move(c)); if (onChanged) onChanged(t); return true;
}
bool History::redo(dna::Document &d) {
  if (redo_.empty()) return false;
  auto c = std::move(redo_.back()); redo_.pop_back(); c->redo(d); auto t = c->touched();
  undo_.push_back(std::move(c)); if (onChanged) onChanged(t); return true;
}
}  // namespace commands
