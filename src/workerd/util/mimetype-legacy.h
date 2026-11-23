// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#pragma once

// The C++ MIME type parser and serializer, which mimetype.h uses unless the RUST_MIMETYPE
// autogate is on. Call mimetype.h instead; this exists for the autogate and for
// mimetype-legacy-test.c++, and goes once the gate is removed.

#include <workerd/util/mimetype.h>

namespace workerd::legacy {

kj::Maybe<MimeType> tryParseMimeType(
    kj::ArrayPtr<const char> input, MimeType::ParseOptions options = MimeType::DEFAULT);
kj::Maybe<MimeType> extractMimeType(kj::StringPtr input);
kj::String mimeTypeToString(const MimeType& mimeType);
kj::String mimeTypeParamsToString(const MimeType& mimeType);

// Whether `s` is a valid type, subtype or parameter name.
bool isMimeToken(kj::ArrayPtr<const char> s);
// Whether `s` is a valid parameter value.
bool isMimeParamValue(kj::ArrayPtr<const char> s);

}  // namespace workerd::legacy
