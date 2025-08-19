// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#pragma once

#include <workerd/jsg/jsg.h>
#include <workerd/rust/dns/lib.rs.h>

#include <kj/string.h>

namespace workerd::api::node {

class DnsUtil final: public jsg::Object {
 public:
  DnsUtil() = default;
  DnsUtil(jsg::Lock&, const jsg::Url&) {}

  rust::dns::CaaRecord parseCaaRecord(kj::String record);
  rust::dns::NaptrRecord parseNaptrRecord(kj::String record);

  JSG_RESOURCE_TYPE(DnsUtil) {
    JSG_METHOD(parseCaaRecord);
    JSG_METHOD(parseNaptrRecord);
  }
};

#define EW_NODE_DNS_ISOLATE_TYPES api::node::DnsUtil, rust::dns::CaaRecord, rust::dns::NaptrRecord

}  // namespace workerd::api::node
