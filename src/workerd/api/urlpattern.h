// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/jsg/jsg.h>
#include <workerd/jsg/string.h>  // jsg::UsvString

namespace workerd::api {

// An individual compiled component of a URLPattern
struct URLPatternComponent {
  jsg::UsvString pattern;
  jsg::JsRef<jsg::JsRegExp> regex;
  kj::Array<jsg::UsvString> nameList;
};

// The collection of compiled patterns for each component of a URLPattern.
struct URLPatternComponents {
  URLPatternComponent protocol;
  URLPatternComponent username;
  URLPatternComponent password;
  URLPatternComponent hostname;
  URLPatternComponent port;
  URLPatternComponent pathname;
  URLPatternComponent search;
  URLPatternComponent hash;
};

// URLPattern is a Web Platform standard API for matching URLs against a
// pattern syntax (think of it as a regular expression for URLs). It is
// defined in https://wicg.github.io/urlpattern.
// More information about the URL Pattern syntax can be found at
// https://developer.mozilla.org/en-US/docs/Web/API/URL_Pattern_API
class URLPattern: public jsg::Object {
public:
  // A structure providing matching patterns for individual components
  // of a URL. When a URLPattern is created, or when a URLPattern is
  // used to match or test against a URL, the input can be given as
  // either a string or a URLPatternInit struct. If a string is given,
  // it will be parsed to create a URLPatternInit. The URLPatternInit
  // API is defined as part of the URLPattern specification.
  struct URLPatternInit {
    jsg::Optional<jsg::UsvString> protocol;
    jsg::Optional<jsg::UsvString> username;
    jsg::Optional<jsg::UsvString> password;
    jsg::Optional<jsg::UsvString> hostname;
    jsg::Optional<jsg::UsvString> port;
    jsg::Optional<jsg::UsvString> pathname;
    jsg::Optional<jsg::UsvString> search;
    jsg::Optional<jsg::UsvString> hash;
    jsg::Optional<jsg::UsvString> baseURL;

    JSG_STRUCT(protocol, username, password, hostname, port, pathname, search, hash, baseURL);
  };

  using URLPatternInput = kj::OneOf<jsg::UsvString, URLPatternInit>;

  // A struct providing the URLPattern matching results for a single
  // URL component. The URLPatternComponentResult is only ever used
  // as a member attribute of a URLPatternResult struct. The
  // URLPatternComponentResult API is defined as part of the URLPattern
  // specification.
  struct URLPatternComponentResult {
    jsg::UsvString input;
    jsg::Dict<jsg::UsvString, jsg::UsvString> groups;

    JSG_STRUCT(input, groups);
  };

  // A struct providing the URLPattern matching results for all
  // components of a URL. The URLPatternResult API is defined as
  // part of the URLPattern specification.
  struct URLPatternResult {
    kj::Array<URLPatternInput> inputs;
    URLPatternComponentResult protocol;
    URLPatternComponentResult username;
    URLPatternComponentResult password;
    URLPatternComponentResult hostname;
    URLPatternComponentResult port;
    URLPatternComponentResult pathname;
    URLPatternComponentResult search;
    URLPatternComponentResult hash;

    JSG_STRUCT(inputs, protocol, username, password, hostname, port, pathname, search, hash);
  };

  explicit URLPattern(
      jsg::Lock& js,
      jsg::Optional<URLPatternInput> input,
      jsg::Optional<jsg::UsvString> baseURL);

  static jsg::Ref<URLPattern> constructor(
      jsg::Lock& js,
      jsg::Optional<URLPatternInput> input,
      jsg::Optional<jsg::UsvString> baseURL);

  kj::Maybe<URLPatternResult> exec(
      jsg::Lock& js,
      jsg::Optional<URLPatternInput> input,
      jsg::Optional<jsg::UsvString> baseURL = nullptr);

  bool test(
      jsg::Lock& js,
      jsg::Optional<URLPatternInput> input,
      jsg::Optional<jsg::UsvString> baseURL = nullptr);

  jsg::UsvStringPtr getProtocol();
  jsg::UsvStringPtr getUsername();
  jsg::UsvStringPtr getPassword();
  jsg::UsvStringPtr getHostname();
  jsg::UsvStringPtr getPort();
  jsg::UsvStringPtr getPathname();
  jsg::UsvStringPtr getSearch();
  jsg::UsvStringPtr getHash();

  JSG_RESOURCE_TYPE(URLPattern) {
    JSG_READONLY_PROTOTYPE_PROPERTY(protocol, getProtocol);
    JSG_READONLY_PROTOTYPE_PROPERTY(username, getUsername);
    JSG_READONLY_PROTOTYPE_PROPERTY(password, getPassword);
    JSG_READONLY_PROTOTYPE_PROPERTY(hostname, getHostname);
    JSG_READONLY_PROTOTYPE_PROPERTY(port, getPort);
    JSG_READONLY_PROTOTYPE_PROPERTY(pathname, getPathname);
    JSG_READONLY_PROTOTYPE_PROPERTY(search, getSearch);
    JSG_READONLY_PROTOTYPE_PROPERTY(hash, getHash);
    JSG_METHOD(test);
    JSG_METHOD(exec);
  }

private:
  URLPatternComponents components;

  void visitForGc(jsg::GcVisitor& visitor);
};

#define EW_URLPATTERN_ISOLATE_TYPES           \
  api::URLPattern,                            \
  api::URLPattern::URLPatternInit,            \
  api::URLPattern::URLPatternComponentResult, \
  api::URLPattern::URLPatternResult

}  // namespace workerd::api
