#include "xml_span.h"
#include <cctype>
#include <fstream>
#include <sstream>

namespace dna {

const XmlNode *XmlNode::child(const std::string &t) const {
  for (auto &c : children) if (c->tag == t) return c.get();
  return nullptr;
}
std::string XmlNode::attr(const std::string &k, const std::string &d) const {
  auto it = attrs.find(k); return it == attrs.end() ? d : it->second;
}
std::string XmlNode::text(const std::string &src) const {
  if (textEnd <= textStart) return "";
  std::string s = src.substr(textStart, textEnd - textStart);
  size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
  return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

static bool nameChar(char c) { return std::isalnum((unsigned char)c) || c == '_' || c == ':' || c == '.' || c == '-'; }

std::unique_ptr<XmlNode> parseXml(const std::string &t) {
  auto root = std::make_unique<XmlNode>(); root->tag = "#doc"; root->end = t.size();
  XmlNode *cur = root.get();
  size_t i = 0, n = t.size();
  bool textOpen = false;
  while (i < n) {
    size_t lt = t.find('<', i);
    if (lt == std::string::npos) break;
    if (textOpen && cur->textEnd == 0) cur->textEnd = lt;
    if (t.compare(lt, 4, "<!--") == 0) { size_t e = t.find("-->", lt + 4); i = e == std::string::npos ? n : e + 3; continue; }
    if (t.compare(lt, 2, "<?") == 0) { size_t e = t.find("?>", lt + 2); i = e == std::string::npos ? n : e + 2; continue; }
    if (t.compare(lt, 9, "<![CDATA[") == 0) { size_t e = t.find("]]>", lt); i = e == std::string::npos ? n : e + 3; continue; }
    if (t.compare(lt, 2, "<!") == 0) { size_t e = t.find('>', lt); i = e == std::string::npos ? n : e + 1; continue; }
    size_t p = lt + 1; bool close = false;
    if (p < n && t[p] == '/') { close = true; p++; }
    size_t ns = p; while (p < n && nameChar(t[p])) p++;
    if (p == ns) { i = lt + 1; continue; }
    std::string tag = t.substr(ns, p - ns);
    // scan to '>' respecting quotes, collecting attributes
    std::map<std::string, std::string> attrs; bool selfc = false;
    while (p < n && t[p] != '>') {
      char c = t[p];
      if (c == '"' || c == '\'') { size_t e = t.find(c, p + 1); p = e == std::string::npos ? n : e + 1; continue; }
      if (c == '/' && p + 1 < n && t[p + 1] == '>') { selfc = true; p++; continue; }
      if (nameChar(c)) {
        size_t as = p; while (p < n && nameChar(t[p])) p++;
        std::string k = t.substr(as, p - as);
        size_t q = p; while (q < n && std::isspace((unsigned char)t[q])) q++;
        if (q < n && t[q] == '=') {
          q++; while (q < n && std::isspace((unsigned char)t[q])) q++;
          if (q < n && (t[q] == '"' || t[q] == '\'')) {
            char qc = t[q]; size_t e = t.find(qc, q + 1); if (e == std::string::npos) e = n;
            attrs[k] = t.substr(q + 1, e - q - 1); p = e + 1; continue;
          }
        }
        continue;
      }
      p++;
    }
    size_t tagEnd = p < n ? p + 1 : n;
    i = tagEnd;
    if (close) {
      while (cur != root.get() && cur->tag != tag) { cur->end = lt; cur = cur->parent; }
      if (cur != root.get()) { cur->end = tagEnd; cur = cur->parent; }
      textOpen = false;
      continue;
    }
    auto node = std::make_unique<XmlNode>();
    node->tag = tag; node->attrs = std::move(attrs); node->start = lt; node->openEnd = tagEnd; node->parent = cur;
    XmlNode *raw = node.get(); cur->children.push_back(std::move(node));
    if (selfc) { raw->end = tagEnd; textOpen = false; }
    else { cur = raw; raw->textStart = tagEnd; textOpen = true; }
  }
  while (cur != root.get()) { cur->end = n; cur = cur->parent; }
  return root;
}

std::string readFile(const std::string &path, bool *ok) {
  std::ifstream f(path, std::ios::binary);
  if (ok) *ok = (bool)f;
  std::stringstream ss; ss << f.rdbuf(); return ss.str();
}
bool writeFile(const std::string &path, const std::string &data) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) return false; f << data; return (bool)f;
}

}  // namespace dna
