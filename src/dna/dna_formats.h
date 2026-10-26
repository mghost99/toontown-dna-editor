// Classic Toontown DNA formats next to the XML one:
//  * .dna  - text DNA source (libpandadna / Panda3D-toontown grammar). Parsed by a span-keeping parser into the
//            same XmlNode tree shape the XML files use (tags/attrs named like the XML schema), so the document
//            model, renderer and span-patching save work unchanged; comments and unknown nodes are kept verbatim.
//  * .pdna - compiled binary DNA (libpandadna layout: "PDNA\n" + flag + "\n" + storage section + component
//            stream, little endian, optionally zlib-compressed). Loaded by decompiling to .dna text; saved by
//            compiling the (edited) tree.
// Pure C++: zlib (compressed .pdna only) is injected by the app via setZlib().
#pragma once
#include "xml_span.h"
#include <functional>
#include <string>

namespace dna {

enum class Format { Xml, Dna, Pdna };

const char *formatName(Format f);
// By extension (.xml/.dna/.pdna), falling back to magic/content sniffing for anything else.
Format detectFormat(const std::string &path, const std::string &bytes);
// Format of an in-memory *text* (xml if it starts with '<', else dna). Used for storage/scene scans.
Format textFormat(const std::string &text);

// Span-keeping text DNA parser. Never throws; tolerant of junk. Returns a '#doc' root.
std::unique_ptr<XmlNode> parseDnaText(const std::string &text, int *errors = nullptr);
// Parses XML or DNA text by content.
std::unique_ptr<XmlNode> parseText(const std::string &text);

// Character data of a node (title/texture/text letters), from XML or DNA sources.
std::string nodeText(const XmlNode *n, const std::string &src);

// Serialisers over the common tree (works for trees parsed from either format).
std::string toDnaText(const XmlNode *n, const std::string &src, int depth = 0);   // one element (or #doc)
std::string toXmlText(const XmlNode *n, const std::string &src, int depth = 0);
std::string dnaDocument(const XmlNode *root, const std::string &src);   // whole file
std::string xmlDocument(const XmlNode *root, const std::string &src);   // whole file (<scene>/<storage> wrapper)

// ---- .pdna ----
using ZFn = std::function<bool(const std::string &in, std::string *out)>;
void setZlib(ZFn inflate, ZFn deflate);
bool haveZlib();
// Decompiles compiled DNA into .dna source text. *compressed reports the header flag.
bool decompilePdna(const std::string &bytes, std::string *dnaText, std::string *err, bool *compressed = nullptr);
struct CompileStats { int components = 0, skipped = 0; std::string skippedTags; };
bool compilePdna(const XmlNode *root, const std::string &src, std::string *bytes, std::string *err,
                 bool compress = false, CompileStats *stats = nullptr);

// ---- encrypted .pdna ----
// There is no documented, standard PDNA encryption; servers that protect .pdna files typically wrap the whole
// file with Panda3D's encrypt_string()/EncryptStream (OpenSSL, password based). The app injects the cipher
// (Panda3D's decrypt_string/encrypt_string) here; a different scheme can be plugged in the same way.
// A .pdna without the "PDNA\n" magic that is not text is treated as encrypted; a password is right when the
// decrypted bytes start with "PDNA\n". The password lives only in this process (never written or logged).
using CryptFn = std::function<bool(const std::string &in, const std::string &password, std::string *out)>;
void setCipher(CryptFn decrypt, CryptFn encrypt);
void setSessionPassword(const std::string &pw);   // in memory only
bool haveSessionPassword();
enum class LoadError { None, Io, NeedPassword, WrongPassword, Format };

// Reads any of the three formats. *text is XML or DNA source (decompiled for .pdna).
bool loadAny(const std::string &path, Format *fmt, std::string *text, std::string *err, bool *compressed = nullptr,
             bool *encrypted = nullptr, LoadError *why = nullptr);
// Encrypts compiled bytes with the session password (for re-saving an encrypted .pdna).
bool encryptWithSession(const std::string &plain, std::string *out, std::string *err);
// Reads + parses any format (storage scans, catalog). nullptr-safe outputs.
std::unique_ptr<XmlNode> parseFile(const std::string &path, std::string *text = nullptr, Format *fmt = nullptr);

}  // namespace dna
