// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#include "mimetype.h"
#include "strings.h"
#include <kj/debug.h>
#include <kj/string-tree.h>
#include <workerd/util/string-buffer.h>

namespace workerd {

namespace {

bool isWhitespace(const char c) {
  return (c == '\r' || c == '\n' || c == '\t' || c == ' ');
}

bool isTokenChar(const unsigned char c) {
  return c == '!' || c == '#' || c == '$' || c == '%' || c == '&' || c == '\'' ||
         c == '*' || c == '+' || c == '\\' || c == '-' || c == '.' || c == '^' ||
         c == '_' || c == '`' || c == '|' || c == '~' || (c >= 'A' && c <= 'Z') ||
         (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

bool isQuotedStringTokenChar(const unsigned char c) {
  return (c == '\t') || (c >= 0x20 && c <= 0x7e) || (c >= 0x80);
}

kj::StringPtr skipWhitespace(kj::StringPtr str) {
  auto ptr = str.begin();
  auto end = str.end();
  while (ptr != end && isWhitespace(*ptr)) { ptr++; }
  return str.slice(ptr - str.begin());
}

kj::ArrayPtr<const char> trimWhitespace(kj::ArrayPtr<const char> str) {
  auto ptr = str.end();
  while (ptr > str.begin() && isWhitespace(*(ptr - 1))) --ptr;
  return str.slice(0, str.size() - (str.end() - ptr));
}

bool hasInvalidCodepoints(kj::ArrayPtr<const char> str, auto predicate) {
  auto ptr = str.begin();
  auto end = str.end();
  while (ptr != end) {
    if (predicate(static_cast<unsigned char>(*ptr))) {
      ++ptr;
      continue;
    }
    return true;
  }
  return false;
}

kj::Maybe<size_t> findParamDelimiter(kj::ArrayPtr<const char> str) {
  auto ptr = str.begin();
  while (ptr != str.end()) {
    if (*ptr == ';' || *ptr == '=') return ptr - str.begin();
    ++ptr;
  }
  return kj::none;
}

kj::String unescape(kj::ArrayPtr<const char> str) {
  auto result = kj::strTree();
  while (str.size() > 0) {
    KJ_IF_SOME(pos, str.findFirst('\\')) {
      result = kj::strTree(kj::mv(result), str.slice(0, pos));
      str = str.slice(pos + 1, str.size());
    } else {
      // No more backslashes
      result = kj::strTree(kj::mv(result), str);
      break;
    }
  }
  return result.flatten();
}

}  // namespace

MimeType MimeType::parse(kj::StringPtr input, ParseOptions options) {
  return KJ_ASSERT_NONNULL(tryParse(input, options));
}

kj::Maybe<MimeType> MimeType::tryParse(kj::StringPtr input, ParseOptions options) {
  // Skip leading whitespace from start
  input = skipWhitespace(input);
  if (input.size() == 0) return kj::none;

  kj::Maybe<kj::String> maybeType;
  // Let's try to find the solidus that separates the type and subtype
  KJ_IF_SOME(n, input.findFirst('/')) {
    auto typeCandidate = input.slice(0, n);
    if (typeCandidate.size() == 0 || hasInvalidCodepoints(typeCandidate, isTokenChar)) {
      return kj::none;
    }
    maybeType = kj::str(typeCandidate);
    input = input.slice(n + 1);
  } else {
    // If the solidus is not found, then it's not a valid mime type
    return kj::none;
  }
  auto& type = KJ_ASSERT_NONNULL(maybeType);

  // If there's nothing else to parse at this point, it's not a valid mime type.
  if (input.size() == 0) return kj::none;

  kj::Maybe<kj::String> maybeSubtype;
  KJ_IF_SOME(n, input.findFirst(';')) {
    // If a semi-colon is found, the subtype is everything up to that point
    // minus trailing whitespace.
    auto subtypeCandidate = trimWhitespace(input.slice(0, n));
    if (subtypeCandidate.size() == 0 || hasInvalidCodepoints(subtypeCandidate, isTokenChar)) {
      return kj::none;
    }
    maybeSubtype = kj::str(subtypeCandidate);
    input = input.slice(n + 1);
  } else {
    auto subtypeCandidate = trimWhitespace(input.asArray());
    if (subtypeCandidate.size() == 0 || hasInvalidCodepoints(subtypeCandidate, isTokenChar)) {
      return kj::none;
    }
    maybeSubtype = kj::str(subtypeCandidate);
    input = input.slice(input.size());
  }
  auto& subtype = KJ_ASSERT_NONNULL(maybeSubtype);

  MimeType result(type, subtype);

  if (!(options & ParseOptions::IGNORE_PARAMS)) {
    // Parse the parameters...
    while (input.size() > 0) {
      input = skipWhitespace(input);
      if (input.size() == 0) break;
      KJ_IF_SOME(n, findParamDelimiter(input)) {
        // If the delimiter found is a ; then the parameter is invalid here, and
        // we will ignore it.
        if (input[n] == ';') {
          input = input.slice(n + 1);
          continue;
        }
        KJ_ASSERT(input[n] == '=');
        auto nameCandidate = input.slice(0, n);
        input = input.slice(n + 1);
        if (nameCandidate.size() == 0 || hasInvalidCodepoints(nameCandidate, isTokenChar)) {
          // The name is invalid, try skipping to the next...
          KJ_IF_SOME(p, input.findFirst(';')) {
            input = input.slice(p + 1);
            continue;
          } else {
            break;
          }
        }

        // Check to see if the value starts off quoted or not.
        if (*input.begin() == '"') {
          input = input.slice(1);
          // Our parameter value is quoted. Next we'll scan up until the next
          // quote or until the end of the string.
          KJ_IF_SOME(p, input.findFirst('"')) {
            auto valueCandidate = input.slice(0, p);
            input = input.slice(p + 1);
            if (hasInvalidCodepoints(valueCandidate, isQuotedStringTokenChar)) {
              continue;
            }
            result.addParam(nameCandidate, unescape(valueCandidate));
            KJ_IF_SOME(y, input.findFirst(';')) {
              input = input.slice(y + 1);
              continue;
            }
          } else if (!hasInvalidCodepoints(input, isQuotedStringTokenChar)) {
            result.addParam(nameCandidate, unescape(input));
          }
          break;
        } else {
          // The parameter is not quoted. Let's scan ahead for the next semi-colon.
          KJ_IF_SOME(p, input.findFirst(';')) {
            auto valueCandidate = trimWhitespace(input.slice(0, p));
            input = input.slice(p + 1);
            if (valueCandidate.size() > 0 &&
                !hasInvalidCodepoints(valueCandidate, isQuotedStringTokenChar)) {
              result.addParam(nameCandidate, valueCandidate);
            }
            continue;
          } else {
            auto valueCandidate = trimWhitespace(input);
            if (valueCandidate.size() > 0 &&
                !hasInvalidCodepoints(valueCandidate, isQuotedStringTokenChar)) {
              result.addParam(nameCandidate, valueCandidate);
            }
          }
          break;
        }
        KJ_ASSERT(false);
      } else {
        // If we got here, we scanned input and did not find a semi-colon or equal
        // sign before hitting the end of the input. We treat the remaining bits as
        // invalid and ignore them.
        break;
      }
    }
  }

  return kj::mv(result);
}

MimeType::MimeType(kj::StringPtr type,
                   kj::StringPtr subtype,
                   kj::Maybe<MimeParams> params)
    : type_(toLowerCopy(type)),
      subtype_(toLowerCopy(subtype)) {
  KJ_IF_SOME(p, params) { params_ = kj::mv(p); }
}

kj::StringPtr MimeType::type() const {
  return type_;
}

bool MimeType::setType(kj::StringPtr type) {
  if (type.size() == 0 || hasInvalidCodepoints(type, isTokenChar)) return false;
  type_ = toLowerCopy(type);
  return true;
}

kj::StringPtr MimeType::subtype() const {
  return subtype_;
}

bool MimeType::setSubtype(kj::StringPtr type) {
  if (type.size() == 0 || hasInvalidCodepoints(type, isTokenChar)) return false;
  subtype_ = toLowerCopy(type);
  return true;
}

const MimeType::MimeParams& MimeType::params() const {
  return params_;
}

bool MimeType::addParam(kj::ArrayPtr<const char> name, kj::ArrayPtr<const char> value) {
  if (name.size() == 0 ||
      hasInvalidCodepoints(name, isTokenChar) ||
      hasInvalidCodepoints(value, isQuotedStringTokenChar)) {
    return false;
  }
  params_.upsert(toLowerCopy(name), kj::str(value), [](auto&, auto&&) {});
  return true;
}

void MimeType::eraseParam(kj::StringPtr name) {
  params_.erase(toLowerCopy(name));
}

kj::String MimeType::essence() const {
  return kj::str(type(), "/", subtype());
}

kj::String MimeType::paramsToString() const {
  ToStringBuffer buffer(512);
  paramsToString(buffer);
  return buffer.toString();
}

void MimeType::paramsToString(MimeType::ToStringBuffer& buffer) const {
  bool first = true;
  for (auto& param : params()) {
    buffer.append(first ? "" : ";", param.key, "=");
    first = false;
    if (param.value.size() == 0) {
      buffer.append("\"\"");
    } else if (hasInvalidCodepoints(param.value, isTokenChar)) {
      auto view = param.value.asPtr();
      buffer.append("\"");
      while (view.size() > 0) {
        KJ_IF_SOME(pos, view.findFirst('"')) {
          buffer.append(view.slice(0, pos), "\\\"");
          view = view.slice(pos + 1);
        } else {
          buffer.append(view);
          view = view.slice(view.size());
        }
      }
      buffer.append("\"");
    } else {
      buffer.append(param.value);
    }
  }
}

kj::String MimeType::toString() const {
  ToStringBuffer buffer(512);
  buffer.append(type(), "/", subtype());
  if (params_.size() > 0) {
    buffer.append(";");
    paramsToString(buffer);
  }
  return buffer.toString();
}

MimeType MimeType::clone(ParseOptions options) const {
  MimeParams copy;
  if (!(options & ParseOptions::IGNORE_PARAMS)) {
    for (const auto& entry : params_) {
      copy.insert(kj::str(entry.key), kj::str(entry.value));
    }
  }
  return MimeType(type_, subtype_, kj::mv(copy));
}

bool MimeType::operator==(const MimeType& other) const {
  return this == &other || (type_ == other.type_ && subtype_ == other.subtype_);
}

MimeType::operator kj::String() const { return toString(); }

kj::String KJ_STRINGIFY(const MimeType& mimeType) {
  return mimeType.toString();
}

const kj::StringPtr MimeType::PLAINTEXT_STRING = "text/plain;charset=UTF-8"_kj;
const MimeType MimeType::PLAINTEXT = MimeType::parse(PLAINTEXT_STRING);
const MimeType MimeType::CSS = MimeType("text"_kj, "css"_kj);
const MimeType MimeType::HTML = MimeType("text"_kj, "html"_kj);
const MimeType MimeType::TEXT_JAVASCRIPT = MimeType("text"_kj, "javascript"_kj);
const MimeType MimeType::JSON = MimeType("application"_kj, "json"_kj);
const MimeType MimeType::FORM_URLENCODED = MimeType("application"_kj, "x-www-form-urlencoded"_kj);
const MimeType MimeType::OCTET_STREAM = MimeType("application"_kj, "octet-stream"_kj);
const MimeType MimeType::XHTML = MimeType("application"_kj, "xhtml+xml"_kj);
const MimeType MimeType::JAVASCRIPT = MimeType("application"_kj, "javascript"_kj);
const MimeType MimeType::XJAVASCRIPT = MimeType("application"_kj, "x-javascript"_kj);
const MimeType MimeType::FORM_DATA = MimeType("multipart"_kj, "form-data"_kj);
const MimeType MimeType::MANIFEST_JSON = MimeType("application"_kj, "manifest+json"_kj);
const MimeType MimeType::VTT = MimeType("text"_kj, "vtt"_kj);
const MimeType MimeType::EVENT_STREAM = MimeType("text"_kj, "event-stream"_kj);

bool MimeType::isText(const MimeType& mimeType) {
  auto type = mimeType.type();
  return type == "text" ||
      isXml(mimeType) ||
      isJson(mimeType) ||
      isJavascript(mimeType);
}

bool MimeType::isXml(const MimeType& mimeType) {
  auto type = mimeType.type();
  auto subtype = mimeType.subtype();
  return (type == "text" || type == "application") &&
         (subtype == "xml" || subtype.endsWith("+xml"));
}

bool MimeType::isJson(const MimeType& mimeType) {
  auto type = mimeType.type();
  auto subtype = mimeType.subtype();
  return (type == "text" || type == "application") &&
         (subtype == "json" || subtype.endsWith("+json"));
}

bool MimeType::isFont(const MimeType& mimeType) {
  auto type = mimeType.type();
  auto subtype = mimeType.subtype();
  return (type == "font" || type == "application") &&
         (subtype.startsWith("font-") || subtype.startsWith("x-font-"));
}

bool MimeType::isJavascript(const MimeType& mimeType) {
  return JAVASCRIPT == mimeType ||
         XJAVASCRIPT == mimeType ||
         TEXT_JAVASCRIPT == mimeType;
}

bool MimeType::isImage(const MimeType& mimeType) {
  return mimeType.type() == "image";
}

bool MimeType::isVideo(const MimeType& mimeType) {
  return mimeType.type() == "video";
}

bool MimeType::isAudio(const MimeType& mimeType) {
  return mimeType.type() == "audio";
}
}  // namespace workerd
