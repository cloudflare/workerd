// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/jsg/jsg.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace workerd::api {

// ada's URLPattern regex provider (ada::url_pattern_regex::regex_concept), over V8 RegExps made
// in the current jsg::Lock with the `v` flag, and `i` for ignore_case. ada calls it while it
// compiles a pattern, and while it matches one.
class UrlPatternRegexProvider {
 public:
  UrlPatternRegexProvider() = default;
  using regex_type = jsg::JsRef<jsg::JsRegExp>;
  // std::nullopt if `pattern` is not a valid RegExp.
  static std::optional<regex_type> create_instance(std::string_view pattern, bool ignore_case);
  // The capture groups of a match, std::nullopt for each that did not participate, or
  // std::nullopt for no match.
  static std::optional<std::vector<std::optional<std::string>>> regex_search(
      std::string_view input, const regex_type& pattern);
  static bool regex_match(std::string_view input, const regex_type& pattern);
};

}  // namespace workerd::api
