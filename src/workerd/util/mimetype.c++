// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Under the RUST_MIMETYPE autogate, MIME types are parsed, validated and serialized by Rust
// (`mimetype/`), and this file adapts its bridge to KJ types; otherwise by the C++ code in
// mimetype-legacy.c++.

#include "mimetype.h"

#include "strings.h"

#include <workerd/util/autogate.h>
#include <workerd/util/mimetype-legacy.h>
#include <workerd/util/mimetype/ffi.rs.h>

#include <kj/debug.h>

namespace workerd {

namespace ffi = rust::mimetype;

namespace {

bool useRust() {
  return util::Autogate::isEnabled(util::AutogateKey::RUST_MIMETYPE);
}

::rust::Slice<const uint8_t> bytes(kj::ArrayPtr<const char> s) {
  return {reinterpret_cast<const uint8_t*>(s.begin()), s.size()};
}

::rust::Slice<uint8_t> mutableBytes(kj::String& s) {
  return {reinterpret_cast<uint8_t*>(s.begin()), s.size()};
}

kj::ArrayPtr<const char> chars(::rust::Slice<const uint8_t> s) {
  return kj::arrayPtr(reinterpret_cast<const char*>(s.data()), s.size());
}

bool isToken(kj::ArrayPtr<const char> s) {
  return useRust() ? ffi::is_token(bytes(s)) : legacy::isMimeToken(s);
}

kj::Maybe<MimeType> toMimeType(ffi::Essence essence, kj::Maybe<MimeType::MimeParams> params) {
  if (essence.type_.empty()) return kj::none;
  return MimeType(toLower(chars(essence.type_)), toLower(chars(essence.subtype)), kj::mv(params));
}

// A MimeType's parameters as the Rust serializer takes them, on the stack for all but MIME types
// with an unusual number of parameters.
class ParamViews {
 public:
  explicit ParamViews(const MimeType::MimeParams& params): views(params.size()) {
    size_t i = 0;
    for (auto& param: params) {
      views[i++] = ffi::Param{.name = bytes(param.key), .value = bytes(param.value)};
    }
  }

  ::rust::Slice<const ffi::Param> get() const {
    return {views.begin(), views.size()};
  }

 private:
  kj::SmallArray<ffi::Param, 8> views;
};

}  // namespace

MimeType MimeType::parse(kj::StringPtr input, ParseOptions options) {
  return KJ_ASSERT_NONNULL(tryParse(input, options));
}

kj::Maybe<MimeType> MimeType::tryParse(kj::ArrayPtr<const char> input, ParseOptions options) {
  if (!useRust()) return legacy::tryParseMimeType(input, options);
  if (options & ParseOptions::IGNORE_PARAMS) {
    return toMimeType(ffi::parse_essence(bytes(input)), kj::none);
  }
  ffi::MimeParamSink sink;
  auto essence = ffi::parse(bytes(input), sink);
  return toMimeType(essence, kj::mv(sink.params));
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
  if (!isToken(type)) return false;
  type_ = toLower(type);
  return true;
}

kj::StringPtr MimeType::subtype() const {
  return subtype_;
}

bool MimeType::setSubtype(kj::StringPtr type) {
  if (!isToken(type)) return false;
  subtype_ = toLower(type);
  return true;
}

const MimeType::MimeParams& MimeType::params() const {
  return params_;
}

bool MimeType::addParam(kj::ArrayPtr<const char> name, kj::ArrayPtr<const char> value) {
  bool valid = useRust() ? ffi::is_token(bytes(name)) && ffi::is_param_value(bytes(value))
                         : legacy::isMimeToken(name) && legacy::isMimeParamValue(value);
  if (!valid) return false;
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
  if (!useRust()) return legacy::mimeTypeParamsToString(*this);
  ParamViews views(params_);
  auto result = kj::heapString(ffi::params_serialized_len(views.get()));
  auto written = ffi::serialize_params(views.get(), mutableBytes(result));
  KJ_ASSERT(written == result.size(), "MIME type parameters serialized to an unexpected length");
  return result;
}

kj::String MimeType::toString() const {
  if (!useRust()) return legacy::mimeTypeToString(*this);
  ParamViews views(params_);
  auto type = bytes(type_);
  auto subtype = bytes(subtype_);
  auto result = kj::heapString(ffi::serialized_len(type, subtype, views.get()));
  auto written = ffi::serialize(type, subtype, views.get(), mutableBytes(result));
  KJ_ASSERT(written == result.size(), "MIME type serialized to an unexpected length");
  return result;
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
  if (!useRust()) return legacy::extractMimeType(input);
  ffi::MimeParamSink sink;
  auto essence = ffi::extract(bytes(input), sink);
  return toMimeType(essence, kj::mv(sink.params));
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
