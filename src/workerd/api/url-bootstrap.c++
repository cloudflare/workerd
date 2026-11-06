// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "url-bootstrap.h"

#include <workerd/jsg/url.h>

namespace workerd::api {

namespace {

jsg::USVString toUsvString(jsg::Lock& js, jsg::JsValue value, kj::StringPtr what) {
  return JSG_REQUIRE_NONNULL(value.tryCast<jsg::JsString>(), TypeError, what, " must be a string")
      .toUSVString(js);
}

kj::Maybe<jsg::USVString> toOptionalUsvString(
    jsg::Lock& js, jsg::JsValue value, kj::StringPtr what) {
  if (value.isUndefined()) return kj::none;
  return toUsvString(js, value, what);
}

kj::ArrayPtr<uint32_t> toComponentsArray(jsg::JsValue out) {
  auto view = JSG_REQUIRE_NONNULL(
      out.tryCast<jsg::JsArrayBufferView>(), TypeError, "out must be a Uint32Array");
  JSG_REQUIRE(view.isUint32Array(), TypeError, "out must be a Uint32Array");
  auto elements = view.asArrayPtr<uint32_t>();
  JSG_REQUIRE(elements.size() == UrlBootstrap::COMPONENTS_LENGTH, TypeError, "out must have ",
      UrlBootstrap::COMPONENTS_LENGTH, " elements");
  return elements;
}

void writeComponents(const jsg::Url& url, kj::ArrayPtr<uint32_t> out) {
  auto c = url.getComponents();
  out[0] = c.protocolEnd;
  out[1] = c.usernameEnd;
  out[2] = c.hostStart;
  out[3] = c.hostEnd;
  out[4] = c.port;
  out[5] = c.pathnameStart;
  out[6] = c.searchStart;
  out[7] = c.hashStart;
  out[8] = static_cast<uint32_t>(url.getSchemeType());
}

jsg::Url parseHref(jsg::Lock& js, jsg::JsValue href) {
  auto str = toUsvString(js, href, "href"_kj);
  return JSG_REQUIRE_NONNULL(jsg::Url::tryParse(str.asPtr()), TypeError, "href must be a URL");
}

UrlBootstrap::Attribute toAttribute(jsg::Lock& js, jsg::JsValue attribute) {
  KJ_IF_SOME(number, attribute.tryCast<jsg::JsUint32>()) {
    KJ_IF_SOME(n, number.value(js)) {
      if (n <= static_cast<uint32_t>(UrlBootstrap::Attribute::HASH)) {
        return static_cast<UrlBootstrap::Attribute>(n);
      }
    }
  }
  JSG_FAIL_REQUIRE(TypeError, "attribute must be a URL attribute number");
}

// Applies the setter, returning false only for an href the href setter rejects.
bool applySetter(jsg::Url& url, UrlBootstrap::Attribute attribute, kj::ArrayPtr<const char> value) {
  using Attribute = UrlBootstrap::Attribute;
  switch (attribute) {
    case Attribute::HREF:
      return url.setHref(value);
    case Attribute::PROTOCOL:
      url.setProtocol(value);
      return true;
    case Attribute::USERNAME:
      url.setUsername(value);
      return true;
    case Attribute::PASSWORD:
      url.setPassword(value);
      return true;
    case Attribute::HOST:
      url.setHost(value);
      return true;
    case Attribute::HOSTNAME:
      url.setHostname(value);
      return true;
    case Attribute::PORT:
      url.setPort(kj::Maybe(value));
      return true;
    case Attribute::PATHNAME:
      url.setPathname(value);
      return true;
    case Attribute::SEARCH:
      url.setSearch(kj::Maybe(value));
      return true;
    case Attribute::HASH:
      url.setHash(kj::Maybe(value));
      return true;
  }
  KJ_UNREACHABLE;
}

}  // namespace

jsg::JsValue UrlBootstrap::parse(
    jsg::Lock& js, jsg::JsValue input, jsg::JsValue base, jsg::JsValue out) {
  auto inputStr = toUsvString(js, input, "input"_kj);
  auto baseStr = toOptionalUsvString(js, base, "base"_kj);
  auto components = toComponentsArray(out);
  auto maybeUrl = jsg::Url::tryParse(
      inputStr.asPtr(), baseStr.map([](jsg::USVString& b) { return b.asPtr(); }));
  KJ_IF_SOME(url, maybeUrl) {
    writeComponents(url, components);
    return js.str(url.getHref());
  }
  return js.undefined();
}

bool UrlBootstrap::canParse(jsg::Lock& js, jsg::JsValue input, jsg::JsValue base) {
  auto inputStr = toUsvString(js, input, "input"_kj);
  auto baseStr = toOptionalUsvString(js, base, "base"_kj);
  return jsg::Url::canParse(
      inputStr.asPtr(), baseStr.map([](jsg::USVString& b) { return b.asPtr(); }));
}

jsg::JsValue UrlBootstrap::update(jsg::Lock& js,
    jsg::JsValue href,
    jsg::JsValue attribute,
    jsg::JsValue value,
    jsg::JsValue out) {
  auto url = parseHref(js, href);
  auto attr = toAttribute(js, attribute);
  auto valueStr = toUsvString(js, value, "value"_kj);
  auto components = toComponentsArray(out);
  if (!applySetter(url, attr, valueStr.asArray())) return js.undefined();
  writeComponents(url, components);
  return js.str(url.getHref());
}

jsg::JsValue UrlBootstrap::origin(jsg::Lock& js, jsg::JsValue href) {
  return js.str(parseHref(js, href).getOrigin());
}

}  // namespace workerd::api
