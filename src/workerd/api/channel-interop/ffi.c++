// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "ffi.h"

#include <workerd/io/io-context/current-with-id.h>

namespace workerd::api::channel_interop {

kj::Maybe<kj::Own<jsg_cpp::CppRef>> fetcher_try_unwrap(v8::Isolate* isolate, jsg_cpp::Local value) {
  return jsg_cpp::cppRefTryUnwrap<Fetcher>(isolate, kj::mv(value));
}

jsg_cpp::MaybeLocal fetcher_wrap(v8::Isolate* isolate, const jsg_cpp::CppRef& ref) {
  return jsg_cpp::cppRefWrap<Fetcher>(isolate, ref);
}

kj::Maybe<kj::Own<jsg_cpp::CppRef>> durable_object_class_try_unwrap(
    v8::Isolate* isolate, jsg_cpp::Local value) {
  return jsg_cpp::cppRefTryUnwrap<DurableObjectClass>(isolate, kj::mv(value));
}

jsg_cpp::MaybeLocal durable_object_class_wrap(v8::Isolate* isolate, const jsg_cpp::CppRef& ref) {
  return jsg_cpp::cppRefWrap<DurableObjectClass>(isolate, ref);
}

// Rust's `&T` of an opaque type is interior-mutable, so the shims may call non-const methods.

bool fetcher_subrequest_channel(v8::Isolate* isolate,
    const Fetcher& fetcher,
    uint64_t ioContextId,
    kj::Maybe<kj::Rc<SubrequestChannel>>& result) {
  return jsg_cpp::callCpp(isolate, [&](jsg::Lock&) {
    auto& ioctx = io_context::currentWithId(ioContextId);
    result = const_cast<Fetcher&>(fetcher).getSubrequestChannel(ioctx);
  });
}

bool fetcher_new(v8::Isolate* isolate,
    uint64_t ioContextId,
    kj::Rc<SubrequestChannel> channel,
    kj::Maybe<kj::Own<jsg_cpp::CppRef>>& result) {
  return jsg_cpp::callCppForRef<Fetcher>(isolate, result, [&](jsg::Lock& js) -> jsg::Ref<Fetcher> {
    auto& ioctx = io_context::currentWithId(ioContextId);
    return js.alloc<Fetcher>(ioctx.addObject(kj::mv(channel)));
  });
}

bool fetcher_new_for_channel_number(v8::Isolate* isolate,
    uint32_t channel,
    RequiresHostAndProtocol requiresHostAndProtocol,
    InHouse inHouse,
    kj::Maybe<kj::Own<jsg_cpp::CppRef>>& result) {
  return jsg_cpp::callCppForRef<Fetcher>(isolate, result, [&](jsg::Lock& js) -> jsg::Ref<Fetcher> {
    return js.alloc<Fetcher>(channel, requiresHostAndProtocol, inHouse == InHouse::YES);
  });
}

bool fetcher_map_virtual_host(
    v8::Isolate* isolate, const Fetcher& fetcher, uint16_t port, ::rust::String& result) {
  return jsg_cpp::callCpp(isolate, [&](jsg::Lock& js) {
    auto hostAndPort = const_cast<Fetcher&>(fetcher).mapVirtualHost(js, port);
    result = ::rust::String(hostAndPort.cStr(), hostAndPort.size());
  });
}

bool subrequest_channel_require_allows_transfer(
    v8::Isolate* isolate, const SubrequestChannel& channel) {
  return jsg_cpp::callCpp(isolate,
      [&](jsg::Lock&) { const_cast<SubrequestChannel&>(channel).requireAllowsTransfer(); });
}

kj::Promise<void> subrequest_channel_evict_for_test(
    kj::Rc<SubrequestChannel> channel, EvictWebSocketMode webSocketMode) {
  auto promise = kj::evalNow([&]() { return channel->evictForTest(webSocketMode); });
  return promise.attach(kj::mv(channel));
}

bool durable_object_class_channel(v8::Isolate* isolate,
    const DurableObjectClass& durableObjectClass,
    uint64_t ioContextId,
    kj::Maybe<kj::Rc<ActorClassChannel>>& result) {
  return jsg_cpp::callCpp(isolate, [&](jsg::Lock&) {
    auto& ioctx = io_context::currentWithId(ioContextId);
    result = const_cast<DurableObjectClass&>(durableObjectClass).getChannel(ioctx);
  });
}

bool durable_object_class_new(v8::Isolate* isolate,
    uint64_t ioContextId,
    kj::Rc<ActorClassChannel> channel,
    kj::Maybe<kj::Own<jsg_cpp::CppRef>>& result) {
  return jsg_cpp::callCppForRef<DurableObjectClass>(
      isolate, result, [&](jsg::Lock& js) -> jsg::Ref<DurableObjectClass> {
    auto& ioctx = io_context::currentWithId(ioContextId);
    return js.alloc<DurableObjectClass>(ioctx.addObject(kj::mv(channel)));
  });
}

bool actor_class_channel_require_allows_transfer(
    v8::Isolate* isolate, const ActorClassChannel& channel) {
  return jsg_cpp::callCpp(isolate,
      [&](jsg::Lock&) { const_cast<ActorClassChannel&>(channel).requireAllowsTransfer(); });
}

}  // namespace workerd::api::channel_interop
