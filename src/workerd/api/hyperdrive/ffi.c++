// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "ffi.h"

#include <workerd/api/global-scope.h>
#include <workerd/api/hyperdrive.h>
#include <workerd/api/sockets.h>
#include <workerd/io/io-context.h>
#include <workerd/util/entropy.h>

namespace workerd::api {

namespace {

kj::String toKj(::rust::Str str) {
  return kj::str(kj::arrayPtr(str.data(), str.size()));
}

ServiceWorkerGlobalScope& currentGlobalScope() {
  return IoContext::current().getCurrentLock().getGlobalScope();
}

}  // namespace

kj::Maybe<kj::Own<rust::jsg_cpp::CppRef>> socket_try_unwrap(
    v8::Isolate* isolate, rust::jsg_cpp::Local value) {
  return rust::jsg_cpp::cppRefTryUnwrap<Socket>(isolate, kj::mv(value));
}

rust::jsg_cpp::MaybeLocal socket_wrap(v8::Isolate* isolate, const rust::jsg_cpp::CppRef& ref) {
  return rust::jsg_cpp::cppRefWrap<Socket>(isolate, ref);
}

void hyperdrive_fill_entropy(::rust::Slice<uint8_t> output) {
  getEntropy(kj::arrayPtr(output.data(), output.size()));
}

bool hyperdrive_has_connect_override(v8::Isolate* isolate, ::rust::Str address, bool& result) {
  return rust::jsg_cpp::callCpp(isolate, [&](jsg::Lock&) {
    result = currentGlobalScope().getConnectOverride(toKj(address)) != kj::none;
  });
}

bool hyperdrive_register_overrides(
    v8::Isolate* isolate, uint32_t channel, ::rust::Str host, ::rust::Str ip, uint16_t port) {
  return rust::jsg_cpp::callCpp(isolate, [&](jsg::Lock&) {
    auto& globalScope = currentGlobalScope();
    auto hostStr = toKj(host);
    auto ipStr = toKj(ip);
    auto connect = [&]() {
      return [channel, host = kj::str(hostStr), port](
                 jsg::Lock& js) { return connectHyperdrive(js, channel, host, port); };
    };
    globalScope.setConnectOverride(kj::str(hostStr, ":", port), connect());
    globalScope.setConnectOverride(kj::str(ipStr, ":", port), connect());
    globalScope.setDnsOverride(kj::mv(hostStr), kj::mv(ipStr));
  });
}

bool hyperdrive_connect(v8::Isolate* isolate,
    uint32_t channel,
    ::rust::Str host,
    uint16_t port,
    kj::Maybe<kj::Own<rust::jsg_cpp::CppRef>>& result) {
  return rust::jsg_cpp::callCppForRef<Socket>(isolate, result,
      [&](jsg::Lock& js) { return connectHyperdrive(js, channel, toKj(host), port); });
}

}  // namespace workerd::api
