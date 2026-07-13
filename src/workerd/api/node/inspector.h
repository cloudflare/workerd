// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

// clang-format off
// jsg.h MUST be included before jsg/inspector.h: jsg/inspector.h declares the
// v8_inspector::StringView stringifier via the KJ_STRINGIFY macro, which is only defined once
// kj/string.h (pulled in transitively by jsg.h) has been included. Keep this order; do not let
// clang-format re-sort these two includes.
#include <workerd/jsg/jsg.h>
#include <workerd/jsg/inspector.h>
// clang-format on

#include <v8-inspector.h>

#include <kj/vector.h>

#include <memory>

namespace workerd::api::node {

// Implements the native half of the experimental, local-dev-only `node:inspector` module. This is
// modeled on Node.js's `JSBindingsConnection` (src/inspector_js_api.cc): it is a thin pipe that
// forwards opaque Chrome DevTools Protocol (CDP) JSON strings between JavaScript and the isolate's
// own V8 inspector. All CDP envelope logic (id/method/params/result/error) lives in the TypeScript
// layer (`src/node/inspector.ts`); this class only moves strings.
//
// A connection connects to the isolate's *own* V8 inspector in-process (same thread, same isolate),
// independent of any DevTools WebSocket session. It is only usable in the open-source `workerd`
// binary in local development (gated by the `enable_nodejs_inspector_local_dev` experimental flag,
// and additionally guarded by `!isMultiTenantProcess()` so it can never function in Cloudflare's
// production runtime).
class InspectorConnection final: public jsg::Object {
 public:
  // Called once per inbound CDP message (response or notification) with the raw JSON string.
  using MessageCallback = jsg::Function<void(kj::String)>;

  InspectorConnection(jsg::Lock& js, MessageCallback callback);
  KJ_DISALLOW_COPY_AND_MOVE(InspectorConnection);

  static jsg::Ref<InspectorConnection> constructor(jsg::Lock& js, MessageCallback callback);

  // Dispatch a CDP message (JSON string) to the V8 inspector session. Any responses/notifications
  // the session produces synchronously are delivered to the message callback before this returns.
  void dispatch(jsg::Lock& js, kj::String message);

  // Tear down the V8 inspector session. Safe to call multiple times.
  void disconnect(jsg::Lock& js);

  JSG_RESOURCE_TYPE(InspectorConnection) {
    JSG_METHOD(dispatch);
    JSG_METHOD(disconnect);
  }

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(callback);
  }

 private:
  // Receives CDP messages from the V8 inspector. We buffer them into `connection.outgoing` rather
  // than invoking the JS callback directly, so that we never re-enter JavaScript in the middle of
  // V8InspectorSession::dispatchProtocolMessage(). The buffer is flushed to JS immediately after
  // dispatch returns (see InspectorConnection::flush). Note: messages emitted asynchronously
  // (outside a dispatch() call) are delivered on the next dispatch(); for the Profiler/coverage
  // use case all responses are synchronous, so this is sufficient.
  class ChannelImpl final: public v8_inspector::V8Inspector::Channel {
   public:
    explicit ChannelImpl(InspectorConnection& connection): connection(connection) {}

    void sendResponse(int callId, std::unique_ptr<v8_inspector::StringBuffer> message) override {
      connection.outgoing.add(kj::str(message->string()));
    }
    void sendNotification(std::unique_ptr<v8_inspector::StringBuffer> message) override {
      connection.outgoing.add(kj::str(message->string()));
    }
    void flushProtocolNotifications() override {}

   private:
    // Back-reference to the owning InspectorConnection. The connection owns this channel via its
    // `kj::Own<ChannelImpl> channel` member, so the channel can never outlive the connection and
    // this reference is valid for the channel's entire lifetime.
    InspectorConnection& connection;
  };

  void flush(jsg::Lock& js);

  MessageCallback callback;
  kj::Vector<kj::String> outgoing;
  // Set while flush() is delivering messages, so a reentrant dispatch() (triggered by a JS callback
  // during delivery) appends to `outgoing` instead of starting a nested, out-of-order drain.
  bool draining = false;
  // Destruction order matters: the V8 inspector session holds a raw pointer to `channel`, so
  // `session` must be destroyed first. Members are destroyed in reverse declaration order, so
  // `channel` is declared before `session`.
  kj::Own<ChannelImpl> channel;
  std::unique_ptr<v8_inspector::V8InspectorSession> session;
};

// The `node-internal:inspector` module object. Exposes the `Connection` constructor used by the
// TypeScript `Session` implementation. Mirrors the shape of Node.js's `internalBinding('inspector')`
// (minus the pieces the TS layer implements itself).
class InspectorModule final: public jsg::Object {
 public:
  InspectorModule() = default;
  InspectorModule(jsg::Lock&, const jsg::Url&) {}

  JSG_RESOURCE_TYPE(InspectorModule) {
    JSG_NESTED_TYPE_NAMED(InspectorConnection, Connection);
  }
};

#define EW_NODE_INSPECTOR_ISOLATE_TYPES api::node::InspectorConnection, api::node::InspectorModule

}  // namespace workerd::api::node
