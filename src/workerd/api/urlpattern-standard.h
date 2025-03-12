// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "ada.h"

#include <workerd/jsg/jsg.h>

#include <kj/array.h>
#include <kj/one-of.h>
#include <kj/string.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace workerd::api {

namespace urlpattern {
#define URL_PATTERN_COMPONENTS(V)                                                                  \
  V(Protocol, protocol)                                                                            \
  V(Username, username)                                                                            \
  V(Password, password)                                                                            \
  V(Hostname, hostname)                                                                            \
  V(Port, port)                                                                                    \
  V(Pathname, pathname)                                                                            \
  V(Search, search)                                                                                \
  V(Hash, hash)

// URLPattern is a Web Platform standard API for matching URLs against a
// pattern syntax (think of it as a regular expression for URLs). It is
// defined in https://wicg.github.io/urlpattern.
// More information about the URL Pattern syntax can be found at
// https://developer.mozilla.org/en-US/docs/Web/API/URL_Pattern_API
class URLPattern final: public jsg::Object {
 public:
  class URLPatternRegexEngine {
   public:
    URLPatternRegexEngine() = default;
    using regex_type = jsg::JsRef<jsg::JsRegExp>;
    static std::optional<regex_type> create_instance(std::string_view pattern, bool ignore_case);
    static std::optional<std::vector<std::optional<std::string>>> regex_search(
        std::string_view input, const regex_type& pattern);
    static bool regex_match(std::string_view input, const regex_type& pattern);
  };

  // A structure providing matching patterns for individual components
  // of a URL. When a URLPattern is created, or when a URLPattern is
  // used to match or test against a URL, the input can be given as
  // either a string or a URLPatternInit struct. If a string is given,
  // it will be parsed to create a URLPatternInit. The URLPatternInit
  // API is defined as part of the URLPattern specification.
  struct URLPatternInit {
#define V(_, name) jsg::Optional<kj::String> name;
    URL_PATTERN_COMPONENTS(V)
#undef V
    jsg::Optional<kj::String> baseURL;

    JSG_STRUCT(protocol, username, password, hostname, port, pathname, search, hash, baseURL);

    ada::url_pattern_init toAdaType() const;
  };

  // A struct providing the URLPattern matching results for a single
  // URL component. The URLPatternComponentResult is only ever used
  // as a member attribute of a URLPatternResult struct. The
  // URLPatternComponentResult API is defined as part of the URLPattern
  // specification.
  struct URLPatternComponentResult final {
    jsg::JsString input;
    jsg::JsObject groups;

    JSG_STRUCT(input, groups);
    JSG_STRUCT_TS_OVERRIDE({
                  input: string;
                  groups: Record<string, string>;
                });
  };

  // A struct providing the URLPattern matching results for all
  // components of a URL. The URLPatternResult API is defined as
  // part of the URLPattern specification.
  struct URLPatternResult final {
    kj::Array<kj::OneOf<jsg::JsString, URLPatternInit>> inputs;
#define V(_, name) URLPatternComponentResult name;
    URL_PATTERN_COMPONENTS(V)
#undef V

    JSG_STRUCT(inputs, protocol, username, password, hostname, port, pathname, search, hash);
  };

  struct URLPatternOptions final {
    jsg::Optional<bool> ignoreCase;

    JSG_STRUCT(ignoreCase);

    ada::url_pattern_options toAdaType() const;
  };

  explicit URLPattern(ada::url_pattern<URLPatternRegexEngine> i): inner(kj::mv(i)) {};

  static jsg::Ref<URLPattern> constructor(jsg::Lock& js,
      jsg::Optional<kj::OneOf<jsg::DOMString, URLPatternInit>> input,
      jsg::Optional<kj::OneOf<jsg::DOMString, URLPatternOptions>> baseURL,
      jsg::Optional<URLPatternOptions> patternOptions);

  kj::Maybe<URLPatternResult> exec(jsg::Lock& js,
      jsg::Optional<kj::OneOf<jsg::DOMString, URLPatternInit>> input,
      jsg::Optional<jsg::DOMString> baseURL);

  bool test(jsg::Optional<kj::OneOf<jsg::DOMString, URLPatternInit>> input,
      jsg::Optional<jsg::DOMString> baseURL);

  bool getHasRegExpGroups() const;

#define V(name, _) kj::StringPtr get##name() const;
  URL_PATTERN_COMPONENTS(V)
#undef V

  JSG_RESOURCE_TYPE(URLPattern) {
#define V(Name, name) JSG_READONLY_PROTOTYPE_PROPERTY(name, get##Name);
    URL_PATTERN_COMPONENTS(V)
#undef V
    JSG_READONLY_PROTOTYPE_PROPERTY(hasRegExpGroups, getHasRegExpGroups);
    JSG_METHOD(test);
    JSG_METHOD(exec);

    JSG_TS_OVERRIDE({
                  get hasRegExpGroups(): boolean;
                });
  }

 private:
  ada::url_pattern<URLPatternRegexEngine> inner;

  static URLPatternInit createURLPatternInit(const ada::url_pattern_init& other);
  static URLPatternComponentResult createURLPatternComponentResult(
      jsg::Lock& js, const ada::url_pattern_component_result& other);
  static URLPatternResult createURLPatternResult(
      jsg::Lock& js, const ada::url_pattern_result& other);
};
}  // namespace urlpattern
#define EW_URLPATTERN_STANDARD_ISOLATE_TYPES                                                       \
  api::urlpattern::URLPattern, api::urlpattern::URLPattern::URLPatternInit,                        \
      api::urlpattern::URLPattern::URLPatternComponentResult,                                      \
      api::urlpattern::URLPattern::URLPatternResult,                                               \
      api::urlpattern::URLPattern::URLPatternOptions

}  // namespace workerd::api
