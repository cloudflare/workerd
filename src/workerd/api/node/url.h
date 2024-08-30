// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#pragma once

#include <kj/string.h>
#include <workerd/jsg/jsg.h>

namespace workerd::api::node {

class UrlUtil final: public jsg::Object {
public:
  UrlUtil() = default;
  UrlUtil(jsg::Lock&, const jsg::Url&) {}

  jsg::JsString domainToUnicode(jsg::Lock& js, kj::String domain);
  jsg::JsString domainToASCII(jsg::Lock& js, kj::String domain);

  JSG_RESOURCE_TYPE(UrlUtil) {
    JSG_METHOD(domainToUnicode);
    JSG_METHOD(domainToASCII);
  }
};

#define EW_NODE_URL_ISOLATE_TYPES api::node::UrlUtil

}  // namespace workerd::api::node
