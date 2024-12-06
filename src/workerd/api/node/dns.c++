// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#include "dns.h"

#include <workerd/jsg/exception.h>
#include <workerd/rust/cxx-integration/cxx-bridge.h>
#include <workerd/rust/dns/lib.rs.h>

namespace workerd::api::node {

DnsUtil::CaaRecord DnsUtil::parseCaaRecord(kj::String record) {
  auto parsed = rust::dns::parse_caa_record(::rust::Str(record.begin(), record.size()));
  return CaaRecord{
    .critical = parsed.critical, .field = kj::str(parsed.field), .value = kj::str(parsed.value)};
}

DnsUtil::NaptrRecord DnsUtil::parseNaptrRecord(kj::String record) {
  auto parsed = rust::dns::parse_naptr_record(::rust::Str(record.begin(), record.size()));
  return NaptrRecord{
    .flags = kj::str(parsed.flags),
    .service = kj::str(parsed.service),
    .regexp = kj::str(parsed.regexp),
    .replacement = kj::str(parsed.replacement),
    .order = parsed.order,
    .preference = parsed.preference,
  };
}

}  // namespace workerd::api::node
