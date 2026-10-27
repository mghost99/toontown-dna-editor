// Undo/redo over the document's items. Commands only change dna::Item data; the app re-syncs the scene node of
// every item a command touched (onChanged), so the scene graph is never rebuilt for an edit.
#pragma once
#include "dna/dna_document.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace commands {

struct Command {
  virtual ~Command() = default;
  virtual void redo(dna::Document &d) = 0;
  virtual void undo(dna::Document &d) = 0;
  virtual std::vector<int> touched() const = 0;
  virtual std::string label() const = 0;
};

struct TransformCmd : Command {
  std::vector<int> ids; std::vector<dna::Xform> before, after; std::string what = "Move";
  void redo(dna::Document &d) override { for (size_t i = 0; i < ids.size(); i++) d.items[ids[i]].xf = after[i]; }
  void undo(dna::Document &d) override { for (size_t i = 0; i < ids.size(); i++) d.items[ids[i]].xf = before[i]; }
  std::vector<int> touched() const override { return ids; }
  std::string label() const override { return what; }
};

struct DeleteCmd : Command {   // also used (inverted) for add
  std::vector<int> ids; bool add = false;
  void redo(dna::Document &d) override { for (int i : ids) d.items[i].deleted = !add; }
  void undo(dna::Document &d) override { for (int i : ids) d.items[i].deleted = add; }
  std::vector<int> touched() const override { return ids; }
  std::string label() const override { return add ? "Add" : "Delete"; }
};

struct SourceCmd : Command {
  std::string before, after, what;
  bool beforeDirty = false;
  void redo(dna::Document &d) override { d.replaceSource(0, d.text.size(), after); }
  void undo(dna::Document &d) override { d.replaceSource(0, d.text.size(), before); d.textDirty = beforeDirty; }
  std::vector<int> touched() const override { return {}; }
  std::string label() const override { return what; }
};

class History {
 public:
  std::function<void(const std::vector<int> &)> onChanged;
  // Push an already-applied command (e.g. a drag that has finished).
  void pushApplied(std::unique_ptr<Command> c);
  void execute(dna::Document &d, std::unique_ptr<Command> c);
  bool undo(dna::Document &d);
  bool redo(dna::Document &d);
  bool canUndo() const { return !undo_.empty(); }
  bool canRedo() const { return !redo_.empty(); }
  std::string undoLabel() const { return undo_.empty() ? "" : undo_.back()->label(); }
  std::string redoLabel() const { return redo_.empty() ? "" : redo_.back()->label(); }
  void clear() { undo_.clear(); redo_.clear(); }
  size_t size() const { return undo_.size(); }
 private:
  std::vector<std::unique_ptr<Command>> undo_, redo_;
};

}  // namespace commands
