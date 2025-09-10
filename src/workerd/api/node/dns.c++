// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#include "dns.h"

#include <workerd/jsg/exception.h>
#include <workerd/rust/dns/lib.rs.h>

#include <kj-rs/kj-rs.h>

using namespace kj_rs;

namespace workerd::api::node {

DnsUtil::CaaRecord DnsUtil::parseCaaRecord(kj::String record) {
  // value comes from js so it is always valid utf-8
  auto parsed = rust::dns::parse_caa_record(record.as<RustUncheckedUtf8>());
  return CaaRecord{
    .critical = parsed.critical, .field = kj::str(parsed.field), .value = kj::str(parsed.value)};
}

DnsUtil::NaptrRecord DnsUtil::parseNaptrRecord(kj::String record) {
  // value comes from js so it is always valid utf-8
  auto parsed = rust::dns::parse_naptr_record(record.as<RustUncheckedUtf8>());
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
