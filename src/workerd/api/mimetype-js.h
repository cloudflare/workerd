// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#pragma once

// MIME type parsing and serializing for the TypeScript implementations of MIME types: the
// per-isolate bootstrap's (src/per_isolate/mime/) and node:util's (src/node/internal/
// internal_mime.ts). They keep the type, subtype and parameters themselves and leave parsing and
// serializing to the Rust code behind MimeType (workerd/util/mimetype.h), so that they agree with
// the C++ code that parses Content-Type values. JS strings cross to it as UTF-8 slices of one
// buffer, with no MimeType in between.
//
// Parameters cross as a list of names and values: the name of parameter `i` at index `2 * i`,
// and its value at `2 * i + 1`.

#include <workerd/jsg/jsg.h>

namespace workerd::api {

// Parses `input` as a MIME type. Returns an array of the type, the subtype and the parameters
// (`[type, subtype, name0, value0, ...]`), or kj::none if `input` is not a MIME type. The type,
// subtype and parameter names are lowercased, and a repeated parameter keeps its first value.
kj::Maybe<jsg::JsArray> parseMimeTypeToJs(jsg::Lock& js, jsg::JsString input);

// Serializes a MIME type of `paramCount` parameters, listed in `params`, a list as described
// above. Every index of `params` below `2 * paramCount` must be an own data property holding a
// string, so that reading it runs no user code; a value that is not a string is a TypeError.
jsg::JsString serializeMimeTypeFromJs(jsg::Lock& js,
    jsg::JsString type,
    jsg::JsString subtype,
    jsg::JsObject params,
    uint32_t paramCount);

// Serializes the parameters alone, as the part of a serialized MIME type after the first ';'.
jsg::JsString serializeMimeParamsFromJs(jsg::Lock& js, jsg::JsObject params, uint32_t paramCount);

}  // namespace workerd::api
