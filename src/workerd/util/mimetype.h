// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#pragma once

#include <workerd/jsg/memory.h>

#include <kj/common.h>
#include <kj/exception.h>
#include <kj/map.h>
#include <kj/string.h>

namespace workerd {

template <size_t>
class StringBuffer;

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

  static bool isXml(const MimeType& mimeType);
  static bool isJson(const MimeType& mimeType);
  static bool isFont(const MimeType& mimeType);
  static bool isJavascript(const MimeType& mimeType);
  static bool isImage(const MimeType& mimeType);
  static bool isVideo(const MimeType& mimeType);
  static bool isAudio(const MimeType& mimeType);
  static bool isText(const MimeType& mimeType);

  static const MimeType JSON;
  static const MimeType PLAINTEXT;
  static const MimeType PLAINTEXT_ASCII;
  static const MimeType FORM_URLENCODED;
  static const MimeType FORM_DATA;
  static const MimeType OCTET_STREAM;
  static const MimeType XHTML;
  static const MimeType JAVASCRIPT;
  static const MimeType XJAVASCRIPT;
  static const MimeType HTML;
  static const MimeType CSS;
  static const MimeType TEXT_JAVASCRIPT;
  static const MimeType MANIFEST_JSON;
  static const MimeType VTT;
  static const MimeType EVENT_STREAM;
  static const MimeType WILDCARD;

  // exposed directly for performance reasons
  static const kj::StringPtr PLAINTEXT_STRING;
  static const kj::StringPtr PLAINTEXT_ASCII_STRING;

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

kj::String KJ_STRINGIFY(const MimeType& state);

}  // namespace workerd
