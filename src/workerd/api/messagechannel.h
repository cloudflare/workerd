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
  // There are several variations of MessageEvent implemented in
  // the runtime. This one is specific to MessagePort.
  class MessageEvent final: public Event {
   public:
    static jsg::Ref<MessageEvent> New(jsg::Lock& js,
        jsg::JsRef<jsg::JsValue> data,
        kj::String lastEventId,
        jsg::Ref<MessagePort> source,
        kj::Array<jsg::Ref<MessagePort>> ports);
    static jsg::Ref<MessageEvent> NewError(jsg::Lock& js,
        jsg::JsRef<jsg::JsValue> data,
        kj::String lastEventId,
        jsg::Ref<MessagePort> source,
        kj::Array<jsg::Ref<MessagePort>> ports);

    MessageEvent(kj::String name,
        jsg::JsRef<jsg::JsValue> data,
        kj::String lastEventId,
        jsg::Ref<MessagePort> source,
        kj::Array<jsg::Ref<MessagePort>> ports)
        : Event(kj::mv(name)),
          data(kj::mv(data)),
          lastEventId(kj::mv(lastEventId)),
          source(kj::mv(source)),
          ports(kj::mv(ports)) {}

    static jsg::Ref<MessageEvent> constructor() = delete;

    jsg::JsRef<jsg::JsValue> getData(jsg::Lock& js) {
      return data.addRef(js);
    }
    kj::StringPtr getLastEventId() {
      return lastEventId;
    };
    jsg::Ref<MessagePort> getSource(jsg::Lock& js) {
      return source.addRef();
    };
    kj::ArrayPtr<jsg::Ref<MessagePort>> getPorts(jsg::Lock& js) {
      return ports;
    }

    JSG_RESOURCE_TYPE(MessageEvent) {
      JSG_INHERIT(Event);
      JSG_READONLY_PROTOTYPE_PROPERTY(data, getData);
      JSG_READONLY_PROTOTYPE_PROPERTY(lastEventId, getLastEventId);
      JSG_READONLY_PROTOTYPE_PROPERTY(source, getSource);
      JSG_READONLY_PROTOTYPE_PROPERTY(ports, getPorts);
      // The standard also defines the origin property, but we don't
      // implement origin in this case so we leave it out.
    }

   private:
    jsg::JsRef<jsg::JsValue> data;
    kj::String lastEventId;
    jsg::Ref<MessagePort> source;
    kj::Array<jsg::Ref<MessagePort>> ports;
  };

  // While we do not support transfer lists in the implementation
  // currently, we do want to validate those inputs.
  using TransferList = kj::Array<jsg::JsRef<jsg::JsValue>>;
  struct PostMessageOptions {
    jsg::Optional<TransferList> transfer;
    JSG_STRUCT(transfer);
  };
  using TransferListOrOptions = kj::OneOf<TransferList, PostMessageOptions>;

  MessagePort(IoContext& ioContext);
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

  // jsrpc support
  void serialize(jsg::Lock& js, jsg::Serializer& serializer);
  static jsg::Ref<MessagePort> deserialize(
      jsg::Lock& js, rpc::SerializationTag tag, jsg::Deserializer& deserializer);
  JSG_SERIALIZABLE(rpc::SerializationTag::MESSAGE_PORT);

  jsg::Ref<MessagePort> addRef() {
    return JSG_THIS;
  }
  bool isClosed() const {
    return state.is<Closed>();
  }

  // Delivers a message to this port from the paired port,
  // caching the message if necessary.
  void deliverMessage(jsg::Lock& js, rpc::JsValue::Reader reader);

  // Bind two message ports together such that messages posted to
  // one are delivered to the other.
  static void entangle(MessagePort& port1, MessagePort& port2);

  kj::Maybe<MessagePort&> getOther() {
    return other.map([](auto& port) mutable -> MessagePort& { return *port; });
  }

 private:
  using Pending = kj::Vector<kj::Own<rpc::JsValue::Reader>>;
  struct Started {
    // TODO(message-port): Add the capability for remote delivery.
  };
  struct Closed {};

  IoContext& ioContext;
  kj::OneOf<Pending, Started, Closed> state;
  kj::Maybe<jsg::Ref<MessagePort>> other;
  kj::Maybe<jsg::JsRef<jsg::JsValue>> onmessageValue;

  // The collection of rpcClients known to this port.
  kj::Vector<rpc::JsMessagePort::Client> rpcClients;

  // Send the message to all associated rpc targets.
  kj::Promise<void> sendToRpc(kj::Own<rpc::JsValue::Reader> message);

  // Triggers the message event on the local port.
  void deliver(jsg::Lock& js, kj::Own<rpc::JsValue::Reader> message, jsg::Ref<MessagePort> port);
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
  api::MessagePort, api::MessageChannel, api::MessagePort::PostMessageOptions,                     \
      api::MessagePort::MessageEvent
