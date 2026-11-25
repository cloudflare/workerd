// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

// The per-type and per-method shims through which Rust takes, returns and uses `Fetcher` and
// `DurableObjectClass` (ffi.rs), and the channels behind them. They are C++ API code, which uses
// C++ JSG as the classes do; see src/rust/jsg/README.md, "Safety rules", rule 3.
//
// Every function that takes an isolate takes the isolate of the caller's lock, locked by this
// thread with a context entered and a HandleScope open, and handles in that scope. Each per-method
// shim runs its C++ in jsg_cpp::callCpp() and returns false, with an exception pending, if it
// threw. A shim that takes an `ioContextId` finds the IoContext with io_context::currentWithId(),
// so it fails unless that IoContext is the current one.

#include <workerd/api/actor.h>
#include <workerd/api/http.h>
#include <workerd/io/io-channels.h>
#include <workerd/rust/jsg-cpp/jsg-cpp.h>

#include <rust/cxx.h>

#include <cstdint>

namespace workerd::api::channel_interop {

namespace jsg_cpp = ::workerd::rust::jsg_cpp;

// The nested channel classes, by names in this namespace, which the bridge declares them by.
// Rust holds them as `KjRc`s.
using SubrequestChannel = IoChannelFactory::SubrequestChannel;
using ActorClassChannel = IoChannelFactory::ActorClassChannel;

// The options of a Fetcher on a numbered channel, by names in this namespace, which the bridge
// declares them by as shared enums. C++ `Fetcher` takes whether it is in-house as a bool.
using RequiresHostAndProtocol = Fetcher::RequiresHostAndProtocol;
enum class InHouse { YES, NO };

// Per-type shims of jsg_cpp::CppResource.
kj::Maybe<kj::Own<jsg_cpp::CppRef>> fetcher_try_unwrap(v8::Isolate* isolate, jsg_cpp::Local value);
jsg_cpp::MaybeLocal fetcher_wrap(v8::Isolate* isolate, const jsg_cpp::CppRef& ref);
kj::Maybe<kj::Own<jsg_cpp::CppRef>> durable_object_class_try_unwrap(
    v8::Isolate* isolate, jsg_cpp::Local value);
jsg_cpp::MaybeLocal durable_object_class_wrap(v8::Isolate* isolate, const jsg_cpp::CppRef& ref);

// fetcher.getSubrequestChannel(ioctx).
bool fetcher_subrequest_channel(v8::Isolate* isolate,
    const Fetcher& fetcher,
    uint64_t ioContextId,
    kj::Maybe<kj::Rc<SubrequestChannel>>& result);

// js.alloc<Fetcher>(ioctx.addObject(channel)): a Fetcher bound to the current IoContext.
bool fetcher_new(v8::Isolate* isolate,
    uint64_t ioContextId,
    kj::Rc<SubrequestChannel> channel,
    kj::Maybe<kj::Own<jsg_cpp::CppRef>>& result);

// js.alloc<Fetcher>(channel, requiresHost, isInHouse): a Fetcher on a numbered channel, bound to
// no IoContext, as the environment's bindings are.
bool fetcher_new_for_channel_number(v8::Isolate* isolate,
    uint32_t channel,
    RequiresHostAndProtocol requiresHostAndProtocol,
    InHouse inHouse,
    kj::Maybe<kj::Own<jsg_cpp::CppRef>>& result);

// fetcher.mapVirtualHost(js, port).
bool fetcher_map_virtual_host(
    v8::Isolate* isolate, const Fetcher& fetcher, uint16_t port, ::rust::String& result);

// channel->requireAllowsTransfer().
bool subrequest_channel_require_allows_transfer(
    v8::Isolate* isolate, const SubrequestChannel& channel);

// channel->evictForTest(webSocketMode), which holds the channel until it settles. What it
// throws rejects the promise.
kj::Promise<void> subrequest_channel_evict_for_test(
    kj::Rc<SubrequestChannel> channel, IoChannelFactory::EvictWebSocketMode webSocketMode);

// durableObjectClass.getChannel(ioctx).
bool durable_object_class_channel(v8::Isolate* isolate,
    const DurableObjectClass& durableObjectClass,
    uint64_t ioContextId,
    kj::Maybe<kj::Rc<ActorClassChannel>>& result);

// js.alloc<DurableObjectClass>(ioctx.addObject(channel)): a class bound to the current IoContext.
bool durable_object_class_new(v8::Isolate* isolate,
    uint64_t ioContextId,
    kj::Rc<ActorClassChannel> channel,
    kj::Maybe<kj::Own<jsg_cpp::CppRef>>& result);

// channel->requireAllowsTransfer().
bool actor_class_channel_require_allows_transfer(
    v8::Isolate* isolate, const ActorClassChannel& channel);

}  // namespace workerd::api::channel_interop
