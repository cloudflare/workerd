// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/jsg/jsg.h>

namespace workerd::api {

// The native backend of the per-isolate bootstrap's URLPattern implementation, exposed as utils
// members. Every member is stateless: compiling a pattern returns its components, and the
// TypeScript side matches them itself.
//
// An init is a URLPatternInit as an array of INIT_LENGTH elements: protocol, username, password,
// hostname, port, pathname, search, hash and baseURL, each a string or undefined for absent.
// Every string argument is converted to a USVString here, whatever the caller has done. A
// TypeError is thrown for an argument of the wrong type.
class UrlPatternBootstrap {
 public:
  static constexpr size_t COMPONENT_COUNT = 8;
  static constexpr size_t INIT_LENGTH = COMPONENT_COUNT + 1;

  // What a compiled component is to the matcher, as the numbers the TypeScript side reads.
  // ada's fast paths: a component of any type but REGEXP has no RegExp.
  enum class ComponentType : uint32_t {
    // Matches by its RegExp.
    REGEXP,
    // Matches anything, capturing all of it as its one group, if it has a name.
    FULL_WILDCARD,
    // Matches only its exact value.
    EXACT_MATCH,
    // Matches only the empty string.
    EMPTY,
  };

  // A compiled component's elements: its pattern string, its RegExp (or undefined), the array of
  // its group names, its ComponentType and, for EXACT_MATCH, its value (else '').
  static constexpr size_t COMPONENT_LENGTH = 5;

  // The length of a compiled pattern: whether it has regexp groups, then each component's
  // COMPONENT_LENGTH elements, protocol to hash.
  static constexpr size_t COMPILED_LENGTH = 1 + COMPONENT_COUNT * COMPONENT_LENGTH;

  // Compiles `input` (a string or an init) against `base` (a string, or undefined for none) with
  // ada, with RegExps of the `v` flag, and `i` if `ignoreCase` (a boolean). Throws "Failed to
  // construct URLPattern" for a pattern that does not compile.
  static jsg::JsValue compile(
      jsg::Lock& js, jsg::JsValue input, jsg::JsValue base, jsg::JsValue ignoreCase);

  // The COMPONENT_COUNT strings an init processes to as URL components, as matching processes an
  // init input, the search without its '?'; or undefined if it does not process.
  static jsg::JsValue processInit(jsg::Lock& js, jsg::JsValue init);

  // compile() for the legacy parser (jsg::UrlPattern), with RegExps of the `u` flag. Every
  // component is REGEXP, and hasRegExpGroups false. Throws the parser's TypeError, or "Invalid
  // regular expression syntax.".
  static jsg::JsValue compileLegacy(
      jsg::Lock& js, jsg::JsValue input, jsg::JsValue base, jsg::JsValue ignoreCase);

  // processInit() for the legacy parser, which throws its TypeError where processInit() returns
  // undefined.
  static jsg::JsValue processInitLegacy(jsg::Lock& js, jsg::JsValue init);
};

}  // namespace workerd::api
