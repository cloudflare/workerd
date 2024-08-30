// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#include "url.h"

#include "ada.h"

namespace workerd::api::node {

namespace {

// Implementation is used by `domainToASCII` and `domainToUnicode`
kj::Maybe<std::string> GetHostName(kj::StringPtr domain) {
  if (domain.size() == 0) {
    return kj::none;
  }

  // It is important to have an initial value that contains a special scheme.
  // Since it will change the implementation of `set_hostname` according to URL
  // spec.
  auto out = ada::parse<ada::url>("ws://x");
  JSG_REQUIRE(out.has_value(), Error, "URL parsing failed"_kj);
  if (!out->set_hostname({domain.begin(), domain.size()})) {
    return kj::none;
  }
  return out->get_hostname();
}

}  // namespace

jsg::JsString UrlUtil::domainToASCII(jsg::Lock& js, kj::String domain) {
  KJ_IF_SOME(hostname, GetHostName(domain)) {
    return js.str(kj::StringPtr(hostname.data(), hostname.size()));
  }
  return js.str(""_kj);
}

jsg::JsString UrlUtil::domainToUnicode(jsg::Lock& js, kj::String domain) {
  KJ_IF_SOME(hostname, GetHostName(domain)) {
    auto result = ada::idna::to_unicode(hostname);
    return js.str(kj::StringPtr(result.data(), result.size()));
  }

  return js.str(""_kj);
}

}  // namespace workerd::api::node
