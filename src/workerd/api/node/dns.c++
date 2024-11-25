// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#include "dns.h"

namespace workerd::api::node {
kj::Array<kj::String> DnsUtil::getServers(jsg::Lock& js) {
  JSG_FAIL_REQUIRE(Error, "Not implemented"_kj);
}

void DnsUtil::lookup(jsg::Lock& js,
    kj::String hostname,
    jsg::Optional<LookupOptions> options,
    LookupCallback callback) {
  JSG_FAIL_REQUIRE(Error, "Not implemented"_kj);
}

void DnsUtil::lookupService(
    jsg::Lock& js, kj::String address, kj::uint port, LookupServiceCallback callback) {
  JSG_FAIL_REQUIRE(Error, "Not implemented"_kj);
}

void DnsUtil::resolve(
    jsg::Lock& js, kj::String hostname, kj::String rrtype, ResolveCallback callback) {
  JSG_FAIL_REQUIRE(Error, "Not implemented"_kj);
}

void DnsUtil::resolve4(
    jsg::Lock& js, kj::String hostname, Resolve4Options options, Resolve6Callback callback) {
  JSG_FAIL_REQUIRE(Error, "Not implemented"_kj);
}

void DnsUtil::resolve6(
    jsg::Lock& js, kj::String hostname, Resolve6Options options, Resolve6Callback callback) {
  JSG_FAIL_REQUIRE(Error, "Not implemented"_kj);
}

void DnsUtil::resolveAny(jsg::Lock& js, kj::String hostname, ResolveAnyCallback callback) {
  JSG_FAIL_REQUIRE(Error, "Not implemented"_kj);
}

void DnsUtil::resolveCname(jsg::Lock& js, kj::String hostname, ResolveCnameCallback callback) {
  JSG_FAIL_REQUIRE(Error, "Not implemented"_kj);
}

void DnsUtil::resolveCaa(jsg::Lock& js, kj::String hostname, ResolveCaaCallback callback) {
  JSG_FAIL_REQUIRE(Error, "Not implemented"_kj);
}

void DnsUtil::resolveMx(jsg::Lock& js, kj::String hostname, ResolveMxCallback callback) {
  JSG_FAIL_REQUIRE(Error, "Not implemented"_kj);
}

void DnsUtil::resolveNaptr(jsg::Lock& js, kj::String hostname, ResolveNaptrCallback callback) {
  JSG_FAIL_REQUIRE(Error, "Not implemented"_kj);
}

void DnsUtil::resolveNs(jsg::Lock& js, kj::String hostname, ResolveNsCallback callback) {
  JSG_FAIL_REQUIRE(Error, "Not implemented"_kj);
}

void DnsUtil::resolvePtr(jsg::Lock& js, kj::String hostname, ResolvePtrCallback callback) {
  JSG_FAIL_REQUIRE(Error, "Not implemented"_kj);
}

void DnsUtil::resolveSoa(jsg::Lock& js, kj::String hostname, ResolveSoaCallback callback) {
  JSG_FAIL_REQUIRE(Error, "Not implemented"_kj);
}

void DnsUtil::resolveSrv(jsg::Lock& js, kj::String hostname, ResolveSrvCallback callback) {
  JSG_FAIL_REQUIRE(Error, "Not implemented"_kj);
}

void DnsUtil::resolveTxt(jsg::Lock& js, kj::String hostname, ResolveTxtCallback callback) {
  JSG_FAIL_REQUIRE(Error, "Not implemented"_kj);
}

void DnsUtil::reverse(jsg::Lock& js, kj::String ip, ReverseCallback callback) {
  JSG_FAIL_REQUIRE(Error, "Not implemented"_kj);
}

void DnsUtil::setDefaultResultOrder(jsg::Lock& js, kj::String order) {
  JSG_FAIL_REQUIRE(Error, "Not implemented"_kj);
}

kj::StringPtr DnsUtil::getDefaultResultOrder(jsg::Lock& js) {
  JSG_FAIL_REQUIRE(Error, "Not implemented"_kj);
}

void DnsUtil::setServers(kj::Array<kj::String> servers) {
  JSG_FAIL_REQUIRE(Error, "Not implemented"_kj);
}

}  // namespace workerd::api::node
