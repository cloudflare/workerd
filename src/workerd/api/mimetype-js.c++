// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#include "mimetype-js.h"

#include <workerd/util/autogate.h>
#include <workerd/util/mimetype.h>
#include <workerd/util/mimetype/ffi.rs.h>

#include <cstring>

namespace workerd::api {

namespace ffi = rust::mimetype;

namespace {

// Under RUST_MIMETYPE, the strings cross to the Rust parser and serializer as UTF-8 slices, with
// nothing in between; otherwise they go through MimeType, which runs the C++ code.
bool useRust() {
  return util::Autogate::isEnabled(util::AutogateKey::RUST_MIMETYPE);
}

::rust::Slice<const uint8_t> bytes(kj::ArrayPtr<const char> s) {
  return {reinterpret_cast<const uint8_t*>(s.begin()), s.size()};
}

kj::ArrayPtr<const char> chars(::rust::Slice<const uint8_t> s) {
  return kj::arrayPtr(reinterpret_cast<const char*>(s.data()), s.size());
}

jsg::JsString readString(jsg::Lock& js, jsg::JsObject list, uint32_t index) {
  auto value = list.get(js, js.num(static_cast<double>(index)));
  return JSG_REQUIRE_NONNULL(
      value.tryCast<jsg::JsString>(), TypeError, "a MIME parameter list must hold only strings");
}

// The ASCII-lowercased `s` as a JS string. `s` is a token, which is ASCII.
jsg::JsString lowercaseStr(jsg::Lock& js, kj::ArrayPtr<const char> s) {
  kj::SmallArray<char, 64> lower(s.size());
  for (size_t i = 0; i < s.size(); i++) {
    char c = s[i];
    lower[i] = 'A' <= c && c <= 'Z' ? c + ('a' - 'A') : c;
  }
  return js.str(lower.asPtr());
}

bool equalsIgnoringAsciiCase(kj::ArrayPtr<const char> a, kj::ArrayPtr<const char> b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); i++) {
    char x = a[i], y = b[i];
    if ('A' <= x && x <= 'Z') x += 'a' - 'A';
    if ('A' <= y && y <= 'Z') y += 'a' - 'A';
    if (x != y) return false;
  }
  return true;
}

// Adds the parameters the Rust parser finds to `parts` as MimeType keeps them: the name
// lowercased, and the first value of a name kept.
class JsParamSink final: public ffi::ParamSink {
 public:
  JsParamSink(jsg::Lock& js, kj::Vector<jsg::JsValue>& parts): js(js), parts(parts) {}

  void add(::rust::Slice<const uint8_t> name, ::rust::Slice<const uint8_t> value) override {
    auto nameChars = chars(name);
    for (auto seen: names) {
      if (equalsIgnoringAsciiCase(seen, nameChars)) return;
    }
    // A name is a slice of the input, which outlives the parse.
    names.add(nameChars);
    parts.add(lowercaseStr(js, nameChars));
    parts.add(js.str(chars(value)));
  }

 private:
  jsg::Lock& js;
  kj::Vector<jsg::JsValue>& parts;
  kj::Vector<kj::ArrayPtr<const char>> names;
};

// JS strings as UTF-8, written into one buffer, for the Rust serializer.
class Utf8Strings {
 public:
  explicit Utf8Strings(jsg::Lock& js, kj::ArrayPtr<const jsg::JsString> strings)
      : buffer(totalLength(js, strings)),
        slices(strings.size()) {
    // writeInto() can leave part of the buffer untouched for a string corrupted inside the V8
    // sandbox, so the buffer must not hold leftover memory.
    memset(buffer.begin(), 0, buffer.size());
    size_t offset = 0;
    for (size_t i = 0; i < strings.size(); i++) {
      auto written = strings[i].writeInto(js, buffer.asPtr().slice(offset)).written;
      slices[i] = buffer.asPtr().slice(offset, offset + written);
      offset += written;
    }
  }

  ::rust::Slice<const uint8_t> operator[](size_t i) const {
    return bytes(slices[i]);
  }

 private:
  kj::SmallArray<char, 512> buffer;
  kj::SmallArray<kj::ArrayPtr<const char>, 24> slices;

  static size_t totalLength(jsg::Lock& js, kj::ArrayPtr<const jsg::JsString> strings) {
    size_t total = 0;
    for (auto& s: strings) total += s.utf8Length(js);
    return total;
  }
};

// The names and values in `params`, read once.
kj::Vector<jsg::JsString> readParamStrings(
    jsg::Lock& js, jsg::JsObject params, uint32_t paramCount, size_t extra) {
  kj::Vector<jsg::JsString> strings(extra + 2 * size_t(paramCount));
  for (uint32_t i = 0; i < 2 * paramCount; i++) {
    strings.add(readString(js, params, i));
  }
  return strings;
}

// Views of the first `views.size()` names and values in `utf8`.
void fillParamViews(kj::ArrayPtr<ffi::Param> views, const Utf8Strings& utf8) {
  for (size_t i = 0; i < views.size(); i++) {
    views[i] = ffi::Param{.name = utf8[2 * i], .value = utf8[2 * i + 1]};
  }
}

// Writes `len` bytes with `write`, which returns how many it wrote, and returns them as a JS
// string.
template <typename Write>
jsg::JsString serializeToJs(jsg::Lock& js, size_t len, Write&& write) {
  kj::SmallArray<char, 256> out(len);
  auto written = write(::rust::Slice<uint8_t>(reinterpret_cast<uint8_t*>(out.begin()), len));
  KJ_ASSERT(written == len, "MIME type serialized to an unexpected length");
  return js.str(out.asPtr());
}

MimeType::MimeParams readParams(jsg::Lock& js, jsg::JsObject params, uint32_t paramCount) {
  MimeType::MimeParams result;
  for (uint32_t i = 0; i < paramCount; i++) {
    result.upsert(readString(js, params, 2 * i).toString(js),
        readString(js, params, 2 * i + 1).toString(js), [](auto&, auto&&) {});
  }
  return result;
}

}  // namespace

kj::Maybe<jsg::JsArray> parseMimeTypeToJs(jsg::Lock& js, jsg::JsString input) {
  kj::SmallArray<char, 256> utf8(input.utf8Length(js));
  memset(utf8.begin(), 0, utf8.size());
  auto inputChars = utf8.asPtr().first(input.writeInto(js, utf8.asPtr()).written);

  if (!useRust()) {
    KJ_IF_SOME(parsed, MimeType::tryParse(inputChars)) {
      auto& params = parsed.params();
      kj::Vector<jsg::JsValue> parts(2 + 2 * params.size());
      parts.add(js.str(parsed.type()));
      parts.add(js.str(parsed.subtype()));
      for (auto& param: params) {
        parts.add(js.str(param.key));
        parts.add(js.str(param.value));
      }
      return js.arr(parts.asPtr());
    }
    return kj::none;
  }

  kj::Vector<jsg::JsValue> parts(8);
  // The type and subtype go first, once the parse finds them; the parameters arrive during it.
  parts.add(js.undefined());
  parts.add(js.undefined());
  JsParamSink sink(js, parts);
  auto essence = ffi::parse(bytes(inputChars), sink);
  if (essence.type_.empty()) return kj::none;
  parts[0] = lowercaseStr(js, chars(essence.type_));
  parts[1] = lowercaseStr(js, chars(essence.subtype));
  return js.arr(parts.asPtr());
}

jsg::JsString serializeMimeTypeFromJs(jsg::Lock& js,
    jsg::JsString type,
    jsg::JsString subtype,
    jsg::JsObject params,
    uint32_t paramCount) {
  if (!useRust()) {
    return js.str(
        MimeType(type.toString(js), subtype.toString(js), readParams(js, params, paramCount))
            .toString());
  }
  auto strings = readParamStrings(js, params, paramCount, 2);
  strings.add(type);
  strings.add(subtype);
  Utf8Strings utf8(js, strings);
  kj::SmallArray<ffi::Param, 8> views(paramCount);
  fillParamViews(views, utf8);
  auto typeBytes = utf8[2 * paramCount];
  auto subtypeBytes = utf8[2 * paramCount + 1];
  auto viewSlice = ::rust::Slice<const ffi::Param>(views.begin(), views.size());
  return serializeToJs(
      js, ffi::serialized_len(typeBytes, subtypeBytes, viewSlice), [&](::rust::Slice<uint8_t> out) {
    return ffi::serialize(typeBytes, subtypeBytes, viewSlice, out);
  });
}

jsg::JsString serializeMimeParamsFromJs(jsg::Lock& js, jsg::JsObject params, uint32_t paramCount) {
  if (!useRust()) {
    return js.str(
        MimeType(kj::String(), kj::String(), readParams(js, params, paramCount)).paramsToString());
  }
  auto strings = readParamStrings(js, params, paramCount, 0);
  Utf8Strings utf8(js, strings);
  kj::SmallArray<ffi::Param, 8> views(paramCount);
  fillParamViews(views, utf8);
  auto viewSlice = ::rust::Slice<const ffi::Param>(views.begin(), views.size());
  return serializeToJs(js, ffi::params_serialized_len(viewSlice),
      [&](::rust::Slice<uint8_t> out) { return ffi::serialize_params(viewSlice, out); });
}

}  // namespace workerd::api
