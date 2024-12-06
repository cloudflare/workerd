// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#pragma once

#include <workerd/jsg/jsg.h>

#include <kj/string.h>

#include <cstdint>

namespace workerd::api::node {

class DnsUtil final: public jsg::Object {
 public:
  DnsUtil() = default;
  DnsUtil(jsg::Lock&, const jsg::Url&) {}

  // TODO: Remove this once we can expose Rust structs
  struct CaaRecord {
    uint8_t critical;
    kj::String field;
    kj::String value;

    JSG_STRUCT(critical, field, value);
  };

  struct NaptrRecord {
    kj::String flags;
    kj::String service;
    kj::String regexp;
    kj::String replacement;
    uint32_t order;
    uint32_t preference;

    JSG_STRUCT(flags, service, regexp, replacement, order, preference);
  };

  CaaRecord parseCaaRecord(kj::String record);
  NaptrRecord parseNaptrRecord(kj::String record);

  JSG_RESOURCE_TYPE(DnsUtil) {
    JSG_METHOD(parseCaaRecord);
    JSG_METHOD(parseNaptrRecord);
  }
};

#define EW_NODE_DNS_ISOLATE_TYPES                                                                  \
  api::node::DnsUtil, api::node::DnsUtil::CaaRecord, api::node::DnsUtil::NaptrRecord

}  // namespace workerd::api::node
