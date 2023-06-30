// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#pragma once

#include <kj/common.h>
#include <kj/exception.h>
#include <kj/map.h>
#include <kj/string.h>

namespace workerd {

class MimeType final {
public:
  using MimeParams = kj::HashMap<kj::String, kj::String>;

  enum ParseOptions {
    DEFAULT,
    IGNORE_PARAMS,
  };

  static kj::Maybe<MimeType> tryParse(kj::StringPtr input,
                                      ParseOptions options = ParseOptions::DEFAULT);
  // Returning nullptr implies that the input is not a valid mime type construction.
  // If the ParseOptions::IGNORE_PARAMS option is set then the mime type parameters
  // will be ignored and will not be included in the parsed result.

  static MimeType parse(kj::StringPtr input,
                        ParseOptions options = ParseOptions::DEFAULT);
  // Asserts if the input could not be parsed as a valid MimeType. tryParse should
  // be preferred for most cases.

  explicit MimeType(kj::StringPtr type,
                    kj::StringPtr subtype,
                    kj::Maybe<MimeParams> params = nullptr);

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

  kj::String essence() const;
  // Returns only the type/subtype

  kj::String toString() const;
  // Returns the type/subtype and all params.

  kj::String paramsToString() const;

  MimeType clone(ParseOptions options = ParseOptions::DEFAULT) const;
  // Copy this MimeType. If the IGNORE_PARAMS option is set the clone
  // will copy only the type and subtype and will omit all of the parameters.

  bool operator==(const MimeType& other) const;
  // Compares only the essence of the MimeType (type and subtype). Ignores
  // parameters in the comparison.

  bool operator!=(const MimeType& other) const;
  // Compares only the essence of the MimeType (type and subtype). Ignores
  // parameters in the comparison.

  operator kj::String() const;

  static bool isXml(const MimeType& mimeType);
  static bool isJson(const MimeType& mimeType);
  static bool isFont(const MimeType& mimeType);
  static bool isJavascript(const MimeType& mimeType);
  static bool isImage(const MimeType& mimeType);
  static bool isVideo(const MimeType& mimeType);
  static bool isAudio(const MimeType& mimeType);

  static const MimeType JSON;
  static const MimeType PLAINTEXT;
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

private:
  kj::String type_;
  kj::String subtype_;
  MimeParams params_;
};

kj::String KJ_STRINGIFY(const MimeType& state);

}  // namespace workerd
