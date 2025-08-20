// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#pragma once

#include <workerd/jsg/jsg.h>
#include <workerd/rust/dns/lib.rs.h>

#include <kj-rs/kj-rs.h>

namespace workerd::api::node {

class DnsUtil final: public jsg::Object {
 public:
  DnsUtil() = default;
  DnsUtil(jsg::Lock&, const jsg::Url&) {}

  using CaaRecord = rust::dns::CaaRecord;
  using NaptrRecord = rust::dns::NaptrRecord;

  JSG_RESOURCE_TYPE(DnsUtil) {
    JSG_RUST_METHOD_NAMED(parseNaptrRecord, rust::dns::parse_naptr_record);
    JSG_RUST_METHOD_NAMED(parseCaaRecord, rust::dns::parse_caa_record);
  }
};

#define EW_NODE_DNS_ISOLATE_TYPES                                                                  \
  api::node::DnsUtil, api::node::DnsUtil::CaaRecord, api::node::DnsUtil::NaptrRecord

}  // namespace workerd::api::node
