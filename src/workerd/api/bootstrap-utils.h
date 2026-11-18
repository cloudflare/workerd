// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/jsg/jsg.h>

namespace workerd::api {

// Helpers that the per-isolate bootstrap's API implementations need from C++, exposed as utils
// members. They are stateless and run no user code.
class BootstrapUtils {
 public:
  // Whether `value` is a Fetcher.
  static bool isFetcher(jsg::Lock& js, jsg::JsValue value);

  // Parses `input` (a string) as an absolute URL, without a base and whichever URL implementation
  // the compat flags give the global. Returns a null-prototype record of its serialization
  // (`href`) and its origin's (`origin`), or undefined if it is not a valid URL. Throws a
  // TypeError if `input` is not a string.
  static jsg::JsValue parseUrl(jsg::Lock& js, jsg::JsValue input);

  // The essence (the lowercased type and subtype, as `type/subtype`) of `input` (a string) parsed
  // as a MIME type, or undefined if it does not parse as one. Throws a TypeError if `input` is not
  // a string.
  static jsg::JsValue mimeTypeEssence(jsg::Lock& js, jsg::JsValue input);

  // The global atob() for an argument already converted: `input` (a string) decoded as
  // forgiving-base64 (WHATWG Infra), each byte a code unit of the result, or undefined if it is
  // not forgiving-base64. Throws a TypeError if `input` is not a string.
  static jsg::JsValue atob(jsg::Lock& js, jsg::JsValue input);

  // The global btoa() for an argument already converted: the base64 encoding of the bytes that
  // the code units of `input` (a string) are, or undefined if a code unit is above 0xFF. Throws a
  // TypeError if `input` is not a string.
  static jsg::JsValue btoa(jsg::Lock& js, jsg::JsValue input);

  // Undefined if `value` is not a ReadableStream of either implementation, else "locked",
  // "disturbed" (unlocked, but read from) or "readable".
  static jsg::JsValue readableStreamState(jsg::Lock& js, jsg::JsValue value);
};

}  // namespace workerd::api
