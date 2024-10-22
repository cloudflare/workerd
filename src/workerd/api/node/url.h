// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#pragma once

#include <workerd/jsg/jsg.h>

#include <kj/string.h>

namespace workerd::api::node {

class UrlUtil final: public jsg::Object {
public:
  UrlUtil() = default;
  UrlUtil(jsg::Lock&, const jsg::Url&) {}

  jsg::JsString domainToUnicode(jsg::Lock& js, kj::String domain);
  jsg::JsString domainToASCII(jsg::Lock& js, kj::String domain);
  jsg::JsString format(
      jsg::Lock& js, kj::String href, bool hash, bool unicode, bool search, bool auth);
  jsg::JsString toASCII(jsg::Lock& js, kj::String url);

  JSG_RESOURCE_TYPE(UrlUtil) {
    JSG_METHOD(domainToUnicode);
    JSG_METHOD(domainToASCII);

    // Legacy APIs
    JSG_METHOD(format);
    JSG_METHOD(toASCII);
  }
};

#define EW_NODE_URL_ISOLATE_TYPES api::node::UrlUtil

}  // namespace workerd::api::node
