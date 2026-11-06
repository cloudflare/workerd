// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/jsg/jsg.h>

namespace workerd::api {

// The native backend of the per-isolate bootstrap's URL implementation, exposed as utils members.
// The ada members are stateless: the TypeScript side keeps a URL as its href and the offsets these
// write, and passes the href back for each change.
//
// Every string argument is converted to a USVString here, whatever the caller has done. A
// TypeError is thrown for an argument of the wrong type.
class UrlBootstrap {
 public:
  // The attributes update() changes, as the numbers the TypeScript side passes.
  enum class Attribute : uint32_t {
    HREF,
    PROTOCOL,
    USERNAME,
    PASSWORD,
    HOST,
    HOSTNAME,
    PORT,
    PATHNAME,
    SEARCH,
    HASH,
  };

  // The number of elements of an `out` array: jsg::Url::Components' eight, then the scheme type
  // (jsg::Url::SchemeType).
  static constexpr size_t COMPONENTS_LENGTH = 9;

  // Parses `input` against `base` (a string, or undefined for none). On success, writes the
  // result's components into `out` (a Uint32Array of COMPONENTS_LENGTH elements) and returns its
  // href; else returns undefined and leaves `out` alone.
  static jsg::JsValue parse(jsg::Lock& js, jsg::JsValue input, jsg::JsValue base, jsg::JsValue out);

  // Whether parse() would succeed, without making a URL.
  static bool canParse(jsg::Lock& js, jsg::JsValue input, jsg::JsValue base);

  // Applies the setter for `attribute` (an Attribute) to `href` (a URL's serialization), with
  // `value`, and writes the result's components into `out`, as parse() does. Returns the new
  // href, or undefined if the href setter rejected the value. The other setters leave the URL
  // unchanged on a value they reject, and so return `href` itself.
  static jsg::JsValue update(jsg::Lock& js,
      jsg::JsValue href,
      jsg::JsValue attribute,
      jsg::JsValue value,
      jsg::JsValue out);

  // The serialization of the origin of the URL `href`.
  static jsg::JsValue origin(jsg::Lock& js, jsg::JsValue href);
};

}  // namespace workerd::api
