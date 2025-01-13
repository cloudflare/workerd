// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/api/basics.h>
#include <workerd/jsg/jsg.h>

#include <kj/array.h>

namespace workerd::api {

// Implements MessagePort web-spec
// Ref: https://html.spec.whatwg.org/multipage/web-messaging.html#message-ports
class MessagePort: public EventTarget {
 public:
  MessagePort() = default;
  ~MessagePort() noexcept(false);
  KJ_DISALLOW_COPY(MessagePort);

  static jsg::Ref<MessagePort> constructor();

  struct StructuredSerializeOptions {
    kj::Array<jsg::Object> transfer{};

    JSG_STRUCT(transfer);
  };

  void postMessage(jsg::Lock& js,
      jsg::Value message,
      kj::OneOf<kj::Maybe<StructuredSerializeOptions>, kj::Array<jsg::Value>> options);
  void start(jsg::Lock& js);
  void stop(jsg::Lock& js);
  void close(jsg::Lock& js);

  JSG_RESOURCE_TYPE(MessagePort) {
    JSG_NESTED_TYPE(EventTarget);
    JSG_METHOD(postMessage);
    JSG_METHOD(start);
    JSG_METHOD(stop);
    JSG_METHOD(close);
  }

  // Ref: https://html.spec.whatwg.org/multipage/web-messaging.html#disentangle
  void disentangle(jsg::Lock& js);

  // Ref: https://html.spec.whatwg.org/multipage/web-messaging.html#entangle
  void entangle(jsg::Lock& js, jsg::Ref<MessagePort> port);

 private:
  bool detached = false;

  kj::Maybe<jsg::HashableV8Ref<v8::Object>> onmessage;
  kj::Maybe<jsg::HashableV8Ref<v8::Object>> onmessageerror;

  // Each MessagePort object can be entangled with another (a symmetric relationship)
  kj::Maybe<jsg::Ref<MessagePort>> entangledWith{};

  class Message {
   public:
    Message() = default;
  };

  bool isMessageQueueEnabled() const {
    return messageQueue != kj::none;
  }

  // Each MessagePort object also has a task source called the port message queue,
  // A port message queue can be enabled or disabled, and is initially disabled.
  // Once enabled, a port can never be disabled again
  kj::Maybe<kj::Vector<Message>> messageQueue{};

  bool hasBeenShipped = false;
};

// Implements MessageChannel web-spec
// Ref: https://html.spec.whatwg.org/multipage/web-messaging.html#message-channels
class MessageChannel: public jsg::Object {
 public:
  explicit MessageChannel(jsg::Lock& js);

  static jsg::Ref<MessageChannel> constructor(jsg::Lock& js);

  jsg::Ref<MessagePort> getPort1() {
    return port1.addRef();
  }

  jsg::Ref<MessagePort> getPort2() {
    return port2.addRef();
  }

  JSG_RESOURCE_TYPE(MessageChannel) {
    JSG_READONLY_PROTOTYPE_PROPERTY(port1, getPort1);
    JSG_READONLY_PROTOTYPE_PROPERTY(port2, getPort2);
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
    tracker.trackField("port1", port1);
    tracker.trackField("port2", port2);
  }

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(port1);
    visitor.visit(port2);
  }

 private:
  jsg::Ref<MessagePort> port1;
  jsg::Ref<MessagePort> port2;
};

#define EW_MESSAGE_CHANNEL_ISOLATE_TYPES                                                           \
  api::MessageChannel, api::MessagePort, api::MessagePort::StructuredSerializeOptions

}  // namespace workerd::api
