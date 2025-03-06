// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#include "dns.h"

#include <workerd/jsg/exception.h>
#include <workerd/rust/cxx-integration/cxx-bridge.h>
#include <workerd/rust/dns/lib.rs.h>

extern "C" v8::Value* parse_naptr_record(v8::Isolate* isolate, const char* str);

namespace workerd::api::node {

DnsUtil::CaaRecord DnsUtil::parseCaaRecord(kj::String record) {
  auto parsed = rust::dns::parse_caa_record(::rust::Str(record.begin(), record.size()));
  return CaaRecord{
    .critical = parsed.critical, .field = kj::str(parsed.field), .value = kj::str(parsed.value)};
}

jsg::Value DnsUtil::parseNaptrRecord(jsg::Lock& js, kj::String record) {
  auto ptr = parse_naptr_record(js.v8Isolate, record.cStr());
  KJ_UNIMPLEMENTED("implement this");
  // return NaptrRecord{
  //   .flags = kj::str(parsed.flags),
  //   .service = kj::str(parsed.service),
  //   .regexp = kj::str(parsed.regexp),
  //   .replacement = kj::str(parsed.replacement),
  //   .order = parsed.order,
  //   .preference = parsed.preference,
  // };
}

}  // namespace workerd::api::node
