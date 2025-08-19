// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#include "dns.h"

#include <workerd/jsg/exception.h>
#include <workerd/rust/dns/lib.rs.h>

#include <kj-rs/kj-rs.h>

using namespace kj_rs;

namespace workerd::api::node {

rust::dns::CaaRecord DnsUtil::parseCaaRecord(kj::String record) {
  // value comes from js so it is always valid utf-8
  return rust::dns::parse_caa_record(record.as<RustUncheckedUtf8>());
}

rust::dns::NaptrRecord DnsUtil::parseNaptrRecord(kj::String record) {
  // value comes from js so it is always valid utf-8
  return rust::dns::parse_naptr_record(record.as<RustUncheckedUtf8>());
}

}  // namespace workerd::api::node
