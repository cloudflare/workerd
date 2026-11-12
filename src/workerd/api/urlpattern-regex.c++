// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "urlpattern-regex.h"

namespace workerd::api {

std::optional<UrlPatternRegexProvider::regex_type> UrlPatternRegexProvider::create_instance(
    std::string_view pattern, bool ignore_case) {
  auto& js = jsg::Lock::current();
  jsg::Lock::RegExpFlags flags = jsg::Lock::RegExpFlags::kUNICODE_SETS;
  if (ignore_case) {
    flags = static_cast<jsg::Lock::RegExpFlags>(
        flags | static_cast<int>(jsg::Lock::RegExpFlags::kIGNORE_CASE));
  }

  // std::string_view is not guaranteed to be null-terminated, but kj::StringPtr requires it.
  // We need to create a null-terminated copy.
  auto str = kj::str(kj::arrayPtr(pattern.data(), pattern.size()));
  JSG_TRY(js) {
    return jsg::JsRef(js, js.regexp(str, flags));
  }
  JSG_CATCH(_) {
    return std::nullopt;
  }
}

bool UrlPatternRegexProvider::regex_match(std::string_view input, const regex_type& pattern) {
  auto& js = jsg::Lock::current();
  // std::string_view is not guaranteed to be null-terminated, but kj::StringPtr requires it.
  // We need to create a null-terminated copy.
  auto str = kj::str(kj::arrayPtr(input.data(), input.size()));
  return pattern.getHandle(js).match(js, str);
}

std::optional<std::vector<std::optional<std::string>>> UrlPatternRegexProvider::regex_search(
    std::string_view input, const regex_type& pattern) {
  auto& js = jsg::Lock::current();
  // std::string_view is not guaranteed to be null-terminated, but kj::StringPtr requires it.
  // We need to create a null-terminated copy.
  auto str = kj::str(kj::arrayPtr(input.data(), input.size()));
  KJ_IF_SOME(matches, pattern.getHandle(js)(js, str)) {
    // Snapshot the array length exactly once. matches.size() calls
    // v8::Array::Length() which reads live JS state — a monkey-patched
    // RegExp.prototype.exec can return an array whose length grows
    // mid-iteration (via accessor getters on indexed properties).
    // Re-reading the length in the loop condition while using operator[]
    // with the pre-allocated size would write past the vector's backing
    // store (heap buffer overflow). Fix: snapshot once, use reserve +
    // emplace_back so we never index past what we allocated.
    const uint32_t len = matches.size();
    if (len == 0) return std::vector<std::optional<std::string>>{};
    std::vector<std::optional<std::string>> results;
    results.reserve(len - 1);
    // The first value is always the input of the exec() command. Therefore
    // we should avoid it while constructing the returning vector.
    for (uint32_t i = 1; i < len; i++) {
      auto value = matches.get(js, i);  // may run user JS via getters
      if (value.isUndefined()) {
        results.emplace_back(std::nullopt);
      } else {
        auto str = value.toString(js);
        results.emplace_back(std::string(str.cStr(), str.size()));
      }
    }
    return kj::mv(results);
  }
  return std::nullopt;
}

}  // namespace workerd::api
