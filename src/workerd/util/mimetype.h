// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#pragma once

#include <workerd/jsg/memory.h>
#include <workerd/util/strings.h>

#include <kj/common.h>
#include <kj/map.h>
#include <kj/string.h>

namespace workerd {

template <size_t>
class StringBuffer;
class MimeType;
class ConstMimeType final {
 public:
  constexpr ConstMimeType(kj::StringPtr type, kj::StringPtr subtype)
      : type_(type),
        subtype_(subtype) {}

  constexpr kj::StringPtr type() const {
    return type_;
  }
  constexpr kj::StringPtr subtype() const {
    return subtype_;
  }

  constexpr bool operator==(const ConstMimeType& other) const {
    return this == &other || (type_ == other.type_ && subtype_ == other.subtype_);
  }

  bool operator==(const MimeType& other) const;
  operator MimeType() const;
  MimeType clone() const;

  inline kj::String toString() const {
    return kj::str(type_, "/", subtype_);
  }

  inline kj::String essence() const {
    return toString();
  }

 private:
  kj::StringPtr type_;
  kj::StringPtr subtype_;
};

template <typename T>
concept IsMimeType = kj::isSameType<T, MimeType>() || kj::isSameType<T, ConstMimeType>();
static_assert(IsMimeType<MimeType>);
static_assert(IsMimeType<ConstMimeType>);

class MimeType final {
 public:
  using MimeParams = kj::HashMap<kj::String, kj::String>;

  enum ParseOptions {
    DEFAULT,
    IGNORE_PARAMS,
  };

  // Returning nullptr implies that the input is not a valid mime type construction.
  // If the ParseOptions::IGNORE_PARAMS option is set then the mime type parameters
  // will be ignored and will not be included in the parsed result.
  static kj::Maybe<MimeType> tryParse(
      kj::ArrayPtr<const char> input, ParseOptions options = ParseOptions::DEFAULT);

  // Asserts if the input could not be parsed as a valid MimeType. tryParse should
  // be preferred for most cases.
  static MimeType parse(kj::StringPtr input, ParseOptions options = ParseOptions::DEFAULT);

  explicit MimeType(
      kj::StringPtr type, kj::StringPtr subtype, kj::Maybe<MimeParams> params = kj::none);
  explicit MimeType(kj::String type, kj::String subtype, kj::Maybe<MimeParams> params = kj::none);

  MimeType(MimeType&&) = default;
  MimeType& operator=(MimeType&&) = default;
  KJ_DISALLOW_COPY(MimeType);

  kj::StringPtr type() const;
  kj::StringPtr subtype() const;

  const MimeParams& params() const;

  bool setType(kj::StringPtr type);
  bool setSubtype(kj::StringPtr type);
  bool addParam(kj::ArrayPtr<const char> name, kj::ArrayPtr<const char> value);
  void eraseParam(kj::StringPtr name);

  // Returns only the type/subtype
  kj::String essence() const;

  // Returns the type/subtype and all params.
  kj::String toString() const;

  kj::String paramsToString() const;

  // Copy this MimeType. If the IGNORE_PARAMS option is set the clone
  // will copy only the type and subtype and will omit all of the parameters.
  MimeType clone(ParseOptions options = ParseOptions::DEFAULT) const;

  // Compares only the essence of the MimeType (type and subtype). Ignores
  // parameters in the comparison.
  bool operator==(const MimeType& other) const;

  operator kj::String() const;

  template <IsMimeType T>
  static constexpr bool isXml(const T& mimeType) {
    auto type = mimeType.type();
    auto subtype = mimeType.subtype();
    return (type == "text" || type == "application") &&
        (subtype == "xml" || subtype.endsWith("+xml"));
  }

  template <IsMimeType T>
  static constexpr bool isJson(const T& mimeType) {
    auto type = mimeType.type();
    auto subtype = mimeType.subtype();
    return (type == "text" || type == "application") &&
        (subtype == "json" || subtype.endsWith("+json"));
  }

  template <IsMimeType T>
  static constexpr bool isFont(const T& mimeType) {
    auto type = mimeType.type();
    auto subtype = mimeType.subtype();
    return (type == "font" || type == "application") &&
        (subtype.startsWith("font-") || subtype.startsWith("x-font-"));
  }

  template <IsMimeType T>
  static constexpr bool isJavascript(const T& mimeType) {
    return JAVASCRIPT == mimeType || XJAVASCRIPT == mimeType || TEXT_JAVASCRIPT == mimeType;
  }

  template <IsMimeType T>
  static constexpr bool isText(const T& mimeType) {
    auto type = mimeType.type();
    auto subtype = mimeType.subtype();
    return type == "text" || isXml(mimeType) || isJson(mimeType) || isJavascript(mimeType) ||
        (type == "application" && subtype == "dns-json");
  }

  template <IsMimeType T>
  static constexpr bool isImage(const T& mimeType) {
    return mimeType.type() == "image";
  }

  template <IsMimeType T>
  static constexpr bool isVideo(const T& mimeType) {
    return mimeType.type() == "video";
  }

  template <IsMimeType T>
  static constexpr bool isAudio(const T& mimeType) {
    return mimeType.type() == "audio";
  }

  static const MimeType PLAINTEXT;
  static const MimeType PLAINTEXT_ASCII;
  static constexpr ConstMimeType JSON = ConstMimeType("application"_kj, "json"_kj);
  static constexpr ConstMimeType FORM_URLENCODED =
      ConstMimeType("application"_kj, "x-www-form-urlencoded"_kj);
  static constexpr ConstMimeType FORM_DATA = ConstMimeType("multipart"_kj, "form-data"_kj);
  static constexpr ConstMimeType OCTET_STREAM = ConstMimeType("application"_kj, "octet-stream"_kj);
  static constexpr ConstMimeType XHTML = ConstMimeType("application"_kj, "xhtml+xml"_kj);
  static constexpr ConstMimeType JAVASCRIPT = ConstMimeType("application"_kj, "javascript"_kj);
  static constexpr ConstMimeType XJAVASCRIPT = ConstMimeType("application"_kj, "x-javascript"_kj);
  static constexpr ConstMimeType TEXT_JAVASCRIPT = ConstMimeType("text"_kj, "javascript"_kj);
  static constexpr ConstMimeType HTML = ConstMimeType("text"_kj, "html"_kj);
  static constexpr ConstMimeType CSS = ConstMimeType("text"_kj, "css"_kj);
  static constexpr ConstMimeType MANIFEST_JSON =
      ConstMimeType("application"_kj, "manifest+json"_kj);
  static constexpr ConstMimeType VTT = ConstMimeType("text"_kj, "vtt"_kj);
  static constexpr ConstMimeType EVENT_STREAM = ConstMimeType("text"_kj, "event-stream"_kj);
  static constexpr ConstMimeType WILDCARD = ConstMimeType("*"_kj, "*"_kj);

  // exposed directly for performance reasons
  static constexpr kj::StringPtr PLAINTEXT_STRING = "text/plain;charset=UTF-8"_kj;
  static constexpr kj::StringPtr PLAINTEXT_ASCII_STRING = "text/plain;charset=US-ASCII"_kj;

  static kj::String formDataWithBoundary(kj::StringPtr boundary);
  static kj::String formUrlEncodedWithCharset(kj::StringPtr charset);

  // Extracts a mime type from a concatenated list of content-type values
  // per the algorithm defined in the fetch spec:
  // https://fetch.spec.whatwg.org/#concept-header-extract-mime-type
  static kj::Maybe<MimeType> extract(kj::StringPtr input);

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
    tracker.trackFieldWithSize("type", type_.size());
    tracker.trackFieldWithSize("subtype", subtype_.size());
    tracker.trackFieldWithSize("params", params_.size());
  }

 private:
  kj::String type_;
  kj::String subtype_;
  MimeParams params_;

  using ToStringBuffer = StringBuffer<128>;
  // 128 bytes will keep all reasonable mimetypes on the stack.

  void paramsToString(ToStringBuffer& buffer) const;

  static kj::Maybe<MimeType> tryParseImpl(
      kj::ArrayPtr<const char> input, ParseOptions options = ParseOptions::DEFAULT);
};

inline ConstMimeType::operator MimeType() const {
  return MimeType(type_, subtype_);
}

inline MimeType ConstMimeType::clone() const {
  return *this;
}

inline bool ConstMimeType::operator==(const MimeType& other) const {
  return type_ == other.type() && subtype_ == other.subtype();
}

kj::String KJ_STRINGIFY(const MimeType& state);
kj::String KJ_STRINGIFY(const ConstMimeType& state);

}  // namespace workerd
