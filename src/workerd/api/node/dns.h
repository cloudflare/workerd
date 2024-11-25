// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#pragma once

#include <workerd/jsg/jsg.h>

#include <kj/string.h>

namespace workerd::api::node {

class DnsUtil final: public jsg::Object {
 public:
  DnsUtil() = default;
  DnsUtil(jsg::Lock&, const jsg::Url&) {}

  kj::Array<kj::String> getServers(jsg::Lock& js);

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

  using ResolveCallback = jsg::Function<void(void)>;
  void resolve(jsg::Lock& js, kj::String hostname, kj::String rrtype, ResolveCallback callback);

  struct Resolve4Options {
    bool ttl;

    JSG_STRUCT(ttl);
  };
  using Resolve4Callback = jsg::Function<void(void)>;
  void resolve4(
      jsg::Lock& js, kj::String hostname, Resolve4Options options, Resolve4Callback callback);

  struct Resolve6Options {
    bool ttl;

    JSG_STRUCT(ttl);
  };
  using Resolve6Callback = jsg::Function<void(void)>;
  void resolve6(
      jsg::Lock& js, kj::String hostname, Resolve6Options options, Resolve6Callback callback);

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

  void setDefaultResultOrder(jsg::Lock& js, kj::String order);
  kj::StringPtr getDefaultResultOrder(jsg::Lock& js);
  void setServers(kj::Array<kj::String> servers);

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
  }
};

#define EW_NODE_DNS_ISOLATE_TYPES                                                                  \
  api::node::DnsUtil, api::node::DnsUtil::LookupOptions, api::node::DnsUtil::Resolve4Options,      \
      api::node::DnsUtil::Resolve6Options

}  // namespace workerd::api::node
