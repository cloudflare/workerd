// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// MIME types are parsed, validated and serialized by the C++ code in mimetype-legacy.c++.

#include "mimetype.h"

#include "strings.h"

#include <workerd/util/mimetype-legacy.h>

#include <kj/debug.h>

namespace workerd {

MimeType MimeType::parse(kj::StringPtr input, ParseOptions options) {
  return KJ_ASSERT_NONNULL(tryParse(input, options));
}

kj::Maybe<MimeType> MimeType::tryParse(kj::ArrayPtr<const char> input, ParseOptions options) {
  return legacy::tryParseMimeType(input, options);
}

MimeType::MimeType(kj::StringPtr type, kj::StringPtr subtype, kj::Maybe<MimeParams> params)
    : MimeType(toLower(type), toLower(subtype), kj::mv(params)) {}

MimeType::MimeType(kj::String type, kj::String subtype, kj::Maybe<MimeParams> params)
    : type_(kj::mv(type)),
      subtype_(kj::mv(subtype)) {
  KJ_IF_SOME(p, params) {
    params_ = kj::mv(p);
  }
}

kj::StringPtr MimeType::type() const {
  return type_;
}

bool MimeType::setType(kj::StringPtr type) {
  if (!legacy::isMimeToken(type)) return false;
  type_ = toLower(type);
  return true;
}

kj::StringPtr MimeType::subtype() const {
  return subtype_;
}

bool MimeType::setSubtype(kj::StringPtr type) {
  if (!legacy::isMimeToken(type)) return false;
  subtype_ = toLower(type);
  return true;
}

const MimeType::MimeParams& MimeType::params() const {
  return params_;
}

bool MimeType::addParam(kj::ArrayPtr<const char> name, kj::ArrayPtr<const char> value) {
  if (!legacy::isMimeToken(name) || !legacy::isMimeParamValue(value)) return false;
  params_.upsert(toLower(name), kj::str(value), [](auto&, auto&&) {});
  return true;
}

void MimeType::eraseParam(kj::StringPtr name) {
  params_.erase(toLower(name));
}

kj::String MimeType::essence() const {
  return kj::str(type(), "/", subtype());
}

kj::String MimeType::paramsToString() const {
  return legacy::mimeTypeParamsToString(*this);
}

kj::String MimeType::toString() const {
  return legacy::mimeTypeToString(*this);
}

MimeType MimeType::clone(ParseOptions options) const {
  MimeParams copy;
  if (!(options & ParseOptions::IGNORE_PARAMS)) {
    for (const auto& entry: params_) {
      copy.insert(kj::str(entry.key), kj::str(entry.value));
    }
  }
  return MimeType(kj::str(type_), kj::str(subtype_), kj::mv(copy));
}

bool MimeType::operator==(const MimeType& other) const {
  return this == &other || (type_ == other.type_ && subtype_ == other.subtype_);
}

MimeType::operator kj::String() const {
  return toString();
}

kj::String KJ_STRINGIFY(const MimeType& mimeType) {
  return mimeType.toString();
}

kj::String KJ_STRINGIFY(const ConstMimeType& state) {
  return state.toString();
}

const MimeType MimeType::PLAINTEXT = MimeType::parse(PLAINTEXT_STRING);
const MimeType MimeType::PLAINTEXT_ASCII = MimeType::parse(PLAINTEXT_ASCII_STRING);

kj::Maybe<MimeType> MimeType::extract(kj::StringPtr input) {
  return legacy::extractMimeType(input);
}

kj::String MimeType::formDataWithBoundary(kj::StringPtr boundary) {
  // Note that the expectation is that the boundary is already properly formed
  // and does not need any additional quoting or escaping.
  return kj::str("multipart/form-data; boundary=", boundary);
}

kj::String MimeType::formUrlEncodedWithCharset(kj::StringPtr charset) {
  return kj::str("application/x-www-form-urlencoded;charset=", charset);
}

}  // namespace workerd
