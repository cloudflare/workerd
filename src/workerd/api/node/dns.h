// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#pragma once

#include <workerd/jsg/jsg.h>

#include <kj/string.h>

namespace workerd::api::node {

class DnsResolverBase {
 public:
  struct ResolveOptions {
    bool ttl;

    JSG_STRUCT(ttl);
  };

  kj::Array<kj::String> getServers(jsg::Lock& js);
  void setServers(kj::Array<kj::String> servers);

  using ResolveCallback = jsg::Function<void(void)>;
  void resolve(jsg::Lock& js, kj::String hostname, kj::String rrtype, ResolveCallback callback);

  using Resolve4Callback = jsg::Function<void(void)>;
  void resolve4(
      jsg::Lock& js, kj::String hostname, ResolveOptions options, Resolve4Callback callback);

  using Resolve6Callback = jsg::Function<void(void)>;
  void resolve6(
      jsg::Lock& js, kj::String hostname, ResolveOptions options, Resolve6Callback callback);

  using ResolveAnyCallback = jsg::Function<void(void)>;
  void resolveAny(jsg::Lock& js, kj::String hostname, ResolveAnyCallback callback);

  using ResolveCnameCallback = jsg::Function<void(void)>;
  void resolveCname(jsg::Lock& js, kj::String hostname, ResolveCnameCallback callback);

  using ResolveCaaCallback = jsg::Function<void(void)>;
  void resolveCaa(jsg::Lock& js, kj::String hostname, ResolveCaaCallback callback);

  using ResolveMxCallback = jsg::Function<void(void)>;
  void resolveMx(jsg::Lock& js, kj::String hostname, ResolveMxCallback callback);

  using ResolveNaptrCallback = jsg::Function<void(void)>;
  void resolveNaptr(jsg::Lock& js, kj::String hostname, ResolveNaptrCallback callback);

  using ResolveNsCallback = jsg::Function<void(void)>;
  void resolveNs(jsg::Lock& js, kj::String hostname, ResolveNsCallback callback);

  using ResolvePtrCallback = jsg::Function<void(void)>;
  void resolvePtr(jsg::Lock& js, kj::String hostname, ResolvePtrCallback callback);

  using ResolveSoaCallback = jsg::Function<void(void)>;
  void resolveSoa(jsg::Lock& js, kj::String hostname, ResolveSoaCallback callback);

  using ResolveSrvCallback = jsg::Function<void(void)>;
  void resolveSrv(jsg::Lock& js, kj::String hostname, ResolveSrvCallback callback);

  using ResolveTxtCallback = jsg::Function<void(void)>;
  void resolveTxt(jsg::Lock& js, kj::String hostname, ResolveTxtCallback callback);

  using ReverseCallback = jsg::Function<void(void)>;
  void reverse(jsg::Lock& js, kj::String ip, ReverseCallback callback);
};

// Implements `node:dns` Resolver class
// Ref: https://nodejs.org/api/dns.html#class-dnsresolver
class DnsResolver final: public DnsResolverBase, public jsg::Object {
 public:
  DnsResolver() = default;
  DnsResolver(jsg::Lock&, const jsg::Url&) {}

  KJ_DISALLOW_COPY_AND_MOVE(DnsResolver);

  struct Options {
    jsg::Optional<int> timeout;
    jsg::Optional<int> tries;

    JSG_STRUCT(timeout, tries);
  };
  static jsg::Ref<DnsResolver> constructor(jsg::Optional<Options> options);

  void cancel(jsg::Lock& js);
  void setLocalAddress(
      jsg::Lock& js, jsg::Optional<kj::String> ipv4, jsg::Optional<kj::String> ipv6);

  JSG_RESOURCE_TYPE(DnsResolver) {
    JSG_METHOD(cancel);
    JSG_METHOD(setLocalAddress);
    JSG_METHOD(getServers);
    JSG_METHOD(setServers);

    JSG_METHOD(resolve);
    JSG_METHOD(resolve4);
    JSG_METHOD(resolve6);
    JSG_METHOD(resolveAny);
    JSG_METHOD(resolveCaa);
    JSG_METHOD(resolveCname);
    JSG_METHOD(resolveMx);
    JSG_METHOD(resolveNaptr);
    JSG_METHOD(resolveNs);
    JSG_METHOD(resolvePtr);
    JSG_METHOD(resolveSoa);
    JSG_METHOD(resolveSrv);
    JSG_METHOD(resolveTxt);
    JSG_METHOD(reverse);
  }
};

class DnsUtil final: public DnsResolverBase, public jsg::Object {
 public:
  DnsUtil() = default;
  DnsUtil(jsg::Lock&, const jsg::Url&) {}

  KJ_DISALLOW_COPY_AND_MOVE(DnsUtil);

  struct LookupOptions {
    kj::OneOf<kj::String, kj::uint> family = static_cast<kj::uint>(0);
    kj::uint hints;
    bool all = false;
    jsg::Optional<kj::String> order;
    bool verbatim = true;

    JSG_STRUCT(family, hints, all, order, verbatim);
  };
  using LookupCallback = jsg::Function<void(void)>;
  void lookup(jsg::Lock& js,
      kj::String hostname,
      jsg::Optional<LookupOptions> options,
      LookupCallback callback);

  using LookupServiceCallback = jsg::Function<void(void)>;
  void lookupService(
      jsg::Lock& js, kj::String address, kj::uint port, LookupServiceCallback callback);

  void setDefaultResultOrder(jsg::Lock& js, kj::String order);
  kj::StringPtr getDefaultResultOrder(jsg::Lock& js);

  JSG_RESOURCE_TYPE(DnsUtil) {
    // Callback implementations
    JSG_METHOD(getServers);
    JSG_METHOD(lookup);
    JSG_METHOD(lookupService);
    JSG_METHOD(resolve);
    JSG_METHOD(resolve4);
    JSG_METHOD(resolve6);
    JSG_METHOD(resolveAny);
    JSG_METHOD(resolveCname);
    JSG_METHOD(resolveCaa);
    JSG_METHOD(resolveMx);
    JSG_METHOD(resolveNaptr);
    JSG_METHOD(resolveNs);
    JSG_METHOD(resolvePtr);
    JSG_METHOD(resolveSoa);
    JSG_METHOD(resolveSrv);
    JSG_METHOD(resolveTxt);
    JSG_METHOD(reverse);

    // Getter, setters
    JSG_METHOD(setDefaultResultOrder);
    JSG_METHOD(getDefaultResultOrder);
    JSG_METHOD(setServers);

    // Classes
    JSG_NESTED_TYPE_NAMED(DnsResolver, Resolver);
  }
};

#define EW_NODE_DNS_ISOLATE_TYPES                                                                  \
  api::node::DnsUtil, api::node::DnsUtil::LookupOptions,                                           \
      api::node::DnsResolverBase::ResolveOptions, api::node::DnsResolver,                          \
      api::node::DnsResolver::Options

}  // namespace workerd::api::node
