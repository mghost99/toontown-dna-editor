// Span-keeping XML tokenizer: every element knows its exact [start, end) in the source text, so saves can patch
// only the changed tags and keep formatting/comments/order. Tolerant of junk (unmatched close tags).
#pragma once
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace dna {

struct XmlNode {
  std::string tag;
  std::map<std::string, std::string> attrs;
  size_t start = 0, openEnd = 0, end = 0;   // [start,openEnd) = open tag; [start,end) = whole element
  size_t textStart = 0, textEnd = 0;         // character data between open and first child/close
  XmlNode *parent = nullptr;
  std::vector<std::unique_ptr<XmlNode>> children;

  const XmlNode *child(const std::string &t) const;
  std::string attr(const std::string &k, const std::string &dflt = "") const;
  std::string text(const std::string &src) const;   // trimmed character data
};

// Parses text; returns a '#doc' root. Never throws.
std::unique_ptr<XmlNode> parseXml(const std::string &text);

// Preorder walk helper.
template <class F> void walk(const XmlNode *n, F &&f) {
  for (auto &c : n->children) { f(c.get()); walk(c.get(), f); }
}

std::string readFile(const std::string &path, bool *ok = nullptr);
bool writeFile(const std::string &path, const std::string &data);

}  // namespace dna
