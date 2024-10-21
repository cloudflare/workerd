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

jsg::JsString UrlUtil::toASCII(jsg::Lock& js, kj::String url) {
  auto out = ada::idna::to_ascii({url.begin(), url.size()});
  return js.str(kj::StringPtr(out.data(), out.size()));
}

jsg::JsString UrlUtil::format(
    jsg::Lock& js, kj::String input, bool hash, bool unicode, bool search, bool auth) {
  // We deliberately use `ada::url` rather than `ada::url_aggregator` because
  // ada::url is faster on setting fields. Since we don't need individual components
  // there is no need to use url_aggregator.
  auto out = ada::parse<ada::url>({input.begin(), input.size()}, nullptr);

  JSG_REQUIRE(out.has_value(), Error, "Failed to parse URL"_kj);

  if (!hash) {
    out->hash = std::nullopt;
  }

  if (unicode && out->has_hostname()) {
    out->host = ada::idna::to_unicode(out->get_hostname());
  }

  if (!search) {
    out->query = std::nullopt;
  }

  if (!auth) {
    out->username = "";
    out->password = "";
  }

  auto href = out->get_href();
  return js.str(kj::StringPtr(href.data(), href.size()));
}

}  // namespace workerd::api::node
