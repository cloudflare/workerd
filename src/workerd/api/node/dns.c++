// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#include "dns.h"

namespace workerd::api::node {
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

void DnsUtil::setDefaultResultOrder(jsg::Lock& js, kj::String order) {
  JSG_FAIL_REQUIRE(Error, "Not implemented"_kj);
}

kj::StringPtr DnsUtil::getDefaultResultOrder(jsg::Lock& js) {
  JSG_FAIL_REQUIRE(Error, "Not implemented"_kj);
}

void DnsResolverBase::resolve(
    jsg::Lock& js, kj::String hostname, kj::String rrtype, ResolveCallback callback) {
  JSG_FAIL_REQUIRE(Error, "Not implemented"_kj);
}

void DnsResolverBase::resolve4(
    jsg::Lock& js, kj::String hostname, ResolveOptions options, Resolve6Callback callback) {
  JSG_FAIL_REQUIRE(Error, "Not implemented"_kj);
}

void DnsResolverBase::resolve6(
    jsg::Lock& js, kj::String hostname, ResolveOptions options, Resolve6Callback callback) {
  JSG_FAIL_REQUIRE(Error, "Not implemented"_kj);
}

void DnsResolverBase::resolveAny(jsg::Lock& js, kj::String hostname, ResolveAnyCallback callback) {
  JSG_FAIL_REQUIRE(Error, "Not implemented"_kj);
}

void DnsResolverBase::resolveCname(
    jsg::Lock& js, kj::String hostname, ResolveCnameCallback callback) {
  JSG_FAIL_REQUIRE(Error, "Not implemented"_kj);
}

void DnsResolverBase::resolveCaa(jsg::Lock& js, kj::String hostname, ResolveCaaCallback callback) {
  JSG_FAIL_REQUIRE(Error, "Not implemented"_kj);
}

void DnsResolverBase::resolveMx(jsg::Lock& js, kj::String hostname, ResolveMxCallback callback) {
  JSG_FAIL_REQUIRE(Error, "Not implemented"_kj);
}

void DnsResolverBase::resolveNaptr(
    jsg::Lock& js, kj::String hostname, ResolveNaptrCallback callback) {
  JSG_FAIL_REQUIRE(Error, "Not implemented"_kj);
}

void DnsResolverBase::resolveNs(jsg::Lock& js, kj::String hostname, ResolveNsCallback callback) {
  JSG_FAIL_REQUIRE(Error, "Not implemented"_kj);
}

void DnsResolverBase::resolvePtr(jsg::Lock& js, kj::String hostname, ResolvePtrCallback callback) {
  JSG_FAIL_REQUIRE(Error, "Not implemented"_kj);
}

void DnsResolverBase::resolveSoa(jsg::Lock& js, kj::String hostname, ResolveSoaCallback callback) {
  JSG_FAIL_REQUIRE(Error, "Not implemented"_kj);
}

void DnsResolverBase::resolveSrv(jsg::Lock& js, kj::String hostname, ResolveSrvCallback callback) {
  JSG_FAIL_REQUIRE(Error, "Not implemented"_kj);
}

void DnsResolverBase::resolveTxt(jsg::Lock& js, kj::String hostname, ResolveTxtCallback callback) {
  JSG_FAIL_REQUIRE(Error, "Not implemented"_kj);
}

void DnsResolverBase::reverse(jsg::Lock& js, kj::String ip, ReverseCallback callback) {
  JSG_FAIL_REQUIRE(Error, "Not implemented"_kj);
}

void DnsResolverBase::setServers(kj::Array<kj::String> servers) {
  JSG_FAIL_REQUIRE(Error, "Not implemented"_kj);
}

kj::Array<kj::String> DnsResolverBase::getServers(jsg::Lock& js) {
  JSG_FAIL_REQUIRE(Error, "Not implemented"_kj);
}

jsg::Ref<DnsResolver> DnsResolver::constructor(jsg::Optional<Options> options) {
  return jsg::alloc<DnsResolver>();
}

void DnsResolver::cancel(jsg::Lock& js) {
  JSG_FAIL_REQUIRE(Error, "Not implemented"_kj);
}

void DnsResolver::setLocalAddress(
    jsg::Lock& js, jsg::Optional<kj::String> ipv4, jsg::Optional<kj::String> ipv6) {
  JSG_FAIL_REQUIRE(Error, "Not implemented"_kj);
}
}  // namespace workerd::api::node
