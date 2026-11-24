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
  // The value of the parameter `name` (matched in lowercase) of the MIME type `input`, or
  // undefined if `input` is not a MIME type or has no such parameter.
  static jsg::JsValue mimeTypeParameter(jsg::Lock& js, jsg::JsValue input, jsg::JsValue name);
  // [readable, writable]: the two ends of a native identity pipe, as C++ HTMLRewriter::transform()
  // makes its transformed body, in the streams implementation in use. A write completes once the
  // readable side has read it, and fails once that side is canceled, with the cancel reason
  // converted as a native stream converts it. Needs a request.
  static jsg::JsValue newIdentityPipe(jsg::Lock& js);
  // Pumps the ReadableStream `stream` (of either implementation) to `sink`, as native code pumps a
  // body to a WritableStreamSink, and returns a promise for the end of the pump. `sink` is an
  // object whose write(ArrayBuffer) and end() return a promise or nothing, and whose
  // abort(reason) takes the reason the pump fails with, converted to JavaScript. A failed write or
  // end cancels the stream with its error, converted through a KJ exception. Needs a request.
  static jsg::JsValue pumpTo(jsg::Lock& js, jsg::JsValue stream, jsg::JsValue sink);

  // Undefined if `value` is not a ReadableStream of either implementation, else "locked",
  // "disturbed" (unlocked, but read from) or "readable".
  static jsg::JsValue readableStreamState(jsg::Lock& js, jsg::JsValue value);
};

}  // namespace workerd::api
