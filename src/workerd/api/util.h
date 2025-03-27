// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/jsg/jsg.h>

#include <v8.h>

#include <kj/async-io.h>
#include <kj/compat/url.h>
#include <kj/string.h>

namespace workerd::api {

// =======================================================================================

#if _MSC_VER
#define strcasecmp _stricmp
#endif

// Case-insensitive comparator for use with std::set/map.
struct CiLess {
  bool operator()(kj::StringPtr lhs, kj::StringPtr rhs) const {
    return strcasecmp(lhs.begin(), rhs.begin()) < 0;
  }
};

// Parse `rawText` as application/x-www-form-urlencoded name/value pairs and store in `query`. If
// `skipLeadingQuestionMark` is true, any initial '?' will be ignored. Otherwise, it will be
// interpreted as part of the first URL-encoded field.
void parseQueryString(kj::Vector<kj::Url::QueryParam>& query,
    kj::ArrayPtr<const char> rawText,
    bool skipLeadingQuestionMark = false);
// TODO(cleanup): Would be really nice to move this to kj-url.

// Given the value of a Content-Type header, returns the value of a single expected parameter.
// For example:
//
//   readContentTypeParameter("application/x-www-form-urlencoded; charset=\"foobar\"", "charset")
//
// would return "foobar" (without the quotes).
//
// Assumptions:
//   - `contentType` has a semi-colon followed by OWS before the parameters.
//   - If the wanted parameter uses quoted-string values, the correct
//     value may not be returned.
kj::Maybe<kj::String> readContentTypeParameter(kj::StringPtr contentType, kj::StringPtr param);
// TODO(cleanup): Replace this function with a full kj::MimeType parser.

// =======================================================================================

struct ErrorTranslation {
  // A snippet of a KJ API exception description to be searched for.
  kj::StringPtr kjDescription;

  // A cleaned up exception description suitable for exposing to JavaScript. There is no need to
  // prefix it with jsg.TypeError.
  kj::StringPtr jsDescription;
};

// HACK: In some cases, KJ APIs throw exceptions with essential details that we want to expose to
// the user, but also sensitive details or poor formatting which we'd prefer not to expose to the
// user. While crude, we can string match to provide cleaned up exception messages. This O(n)
// function helps you do that.
kj::Maybe<kj::Exception> translateKjException(
    const kj::Exception& exception, std::initializer_list<ErrorTranslation> translations);

// =======================================================================================

// Wrap the given stream in an adapter which translates kj::newTee()-specific exceptions into
// JS-visible exceptions.
kj::Own<kj::AsyncInputStream> newTeeErrorAdapter(kj::Own<kj::AsyncInputStream> inner);

// Redacts potential secret keys from a given URL using a couple heuristics:
//   - Any run of hex characters of 32 or more digits, ignoring potential "+-_" separators
//   - Any run of base64 characters of 21 or more digits, including at least
//     two each of digits, capital letters, and lowercase letters.
// Such ids are replaced with the text "REDACTED".
kj::String redactUrl(kj::StringPtr url);

// =======================================================================================

void maybeWarnIfNotText(jsg::Lock& js, kj::StringPtr str);

kj::String fastEncodeBase64Url(kj::ArrayPtr<const byte> bytes);
kj::Array<char16_t> fastEncodeUtf16(kj::ArrayPtr<const char> bytes);

kj::String uriEncodeControlChars(kj::ArrayPtr<const byte> bytes);

}  // namespace workerd::api
