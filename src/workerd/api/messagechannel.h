#pragma once

#include <workerd/api/basics.h>
#include <workerd/io/io-context.h>
#include <workerd/jsg/jsg.h>
#include <workerd/jsg/ser.h>

namespace workerd::api {

// An implementation of the Web platform standard MessagePort.
// MessagePorts always come in pairs. When a message is posted to
// one it is delivered to the other, and vice versa. When one port
// is closed both ports are closed.
class MessagePort final: public EventTarget {
 public:
  // While we do not support transfer lists in the implementation
  // currently, we do want to validate those inputs.
  using TransferList = kj::Array<jsg::JsRef<jsg::JsValue>>;
  struct PostMessageOptions {
    jsg::Optional<TransferList> transfer;
    JSG_STRUCT(transfer);
  };
  using TransferListOrOptions = kj::OneOf<TransferList, PostMessageOptions>;

  MessagePort();
  ~MessagePort() noexcept(false) {
    close();
  }

  // MessagePort instances cannot be created directly.
  // Use `new MessageChannel()`
  static jsg::Ref<MessagePort> constructor() = delete;

  void postMessage(jsg::Lock& js,
      jsg::Optional<jsg::JsRef<jsg::JsValue>> data = kj::none,
      jsg::Optional<TransferListOrOptions> options = kj::none);
  void close();
  void start(jsg::Lock& js);

  // Support the onmessage getter and setter. Per the spec, when
  // onmessage is set, the MessagePort is automatically started,
  // but when addEventListener is set, start must be called
  // separately. That's a kind of a weird rule but ok. To support
  // that we need to define an onmessage getter/setter pair.
  kj::Maybe<jsg::JsValue> getOnMessage(jsg::Lock& js);
  void setOnMessage(jsg::Lock& js, jsg::JsValue value);

  JSG_RESOURCE_TYPE(MessagePort) {
    JSG_INHERIT(EventTarget);
    JSG_METHOD(postMessage);
    JSG_METHOD(close);
    JSG_METHOD(start);
    JSG_PROTOTYPE_PROPERTY(onmessage, getOnMessage, setOnMessage);
  }

  jsg::Ref<MessagePort> addRef() {
    return JSG_THIS;
  }
  bool isClosed() const {
    return state.is<Closed>();
  }

  void deliver(jsg::Lock& js, const jsg::JsValue& data);

  // Bind two message ports together such that messages posted to
  // one are delivered to the other.
  static void entangle(MessagePort& port1, MessagePort& port2);

  kj::Maybe<MessagePort&> getOther() {
    return other.map([](auto& port) mutable -> MessagePort& { return *port; });
  }

  // TODO(soon): Support serialization/deserialization to use MessagePort
  // with JSRPC. We'll need to implement a rpc mechanism for passing the
  // messages across the rpc boundary.

 private:
  // When the MessagePort is in the pending state, messages posted to it
  // will be buffered until the port is started. When the port is started,
  // the buffered messages will be delivered immediately.
  using Pending = kj::Vector<jsg::JsRef<jsg::JsValue>>;
  struct Started {};
  struct Closed {};

  void dispatchMessage(jsg::Lock& js, const jsg::JsValue& value);

  kj::OneOf<Pending, Started, Closed> state;
  kj::Maybe<jsg::Ref<MessagePort>> other;
  kj::Maybe<jsg::JsRef<jsg::JsValue>> onmessageValue;
};

// MessageChannel is simple enough... create a couple of MessagePorts
// and entangle those so that they will exchange messages with each
// other.
class MessageChannel final: public jsg::Object {
 public:
  MessageChannel(jsg::Ref<MessagePort> port1, jsg::Ref<MessagePort> port2)
      : port1(kj::mv(port1)),
        port2(kj::mv(port2)) {}

  static jsg::Ref<MessageChannel> constructor();

  jsg::Ref<MessagePort> getPort1() {
    return port1.addRef();
  }
  jsg::Ref<MessagePort> getPort2() {
    return port2.addRef();
  }

  JSG_RESOURCE_TYPE(MessageChannel) {
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(port1, getPort1);
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(port2, getPort2);
  }

 private:
  jsg::Ref<MessagePort> port1;
  jsg::Ref<MessagePort> port2;
};

}  // namespace workerd::api

#define EW_MESSAGECHANNEL_ISOLATE_TYPES                                                            \
  api::MessagePort, api::MessageChannel, api::MessagePort::PostMessageOptions
