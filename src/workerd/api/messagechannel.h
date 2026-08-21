#pragma once

#include <workerd/api/basics.h>
#include <workerd/io/io-context.h>
#include <workerd/jsg/jsg.h>
#include <workerd/jsg/modules-new.h>
#include <workerd/jsg/ser.h>
#include <workerd/jsg/url.h>

namespace workerd::api {

// A closely approximate implementation of the Web platform standard MessagePort.
// MessagePorts always come in pairs. When a message is posted to
// one it is delivered to the other, and vice versa. When one port
// is closed both ports are closed.
//
// This intentionally does not implement the full MessagePort spec and we know
// that it varies from the standard definition in a number of ways:
//
// - It does not support transfer lists. We do not implement the transfer
//   list semantics, but we do validate the transfer list input to an extent.
// - It does not support serialization/deserialization. It's not possible to
//   send a MessagePort anywhere currently.
// - The `messageerror` event diverges from the spec. If message data cannot be
//   serialized it throws synchronously from postMessage() rather than dispatching
//   `messageerror` on the receiving port; this is easiest for now and makes the
//   most sense for our current use case since the MessagePort only ever passes
//   messages around within the same isolate (that is, we're not sending the
//   serialized data off anywhere, we're just cloning it and dispatching it.)
//   Instead, a throwing 'message' listener — whose exception is reported per
//   spec — additionally dispatches a `messageerror` event on this port carrying
//   the exception as its data, which the spec does not do.
// - We intentionally do not implement the "port message queue" semantics exactly
//   as they are described in the spec. While the port has any 'message' listener —
//   whether assigned to onmessage or added with addEventListener(), which per spec
//   would not enable the queue but does in Node.js — message delivery is flowing;
//   when the last one is removed, messages are queued until another is attached or
//   start() is called. Because we are storing these as JS values, we don't worry
//   about extra memory accounting for the queue.
// - We do not emit the close event on entangled ports when one of them is GC'd.
// - We do not check to see if a MessagePort is entangled with another when we
//   call entangle because there's only one way to entangle them currently and
//   it's impossible for them to be already entangled.
// - We do not implement disentangle steps other than to invalidate the weak
//   ref to the other port when one of them is closed.
// - We do not prevent a MessagePort from being garbage collected while it has
//   messages queued up. Eventually when we implement ser/deser this might change.
// - Unlike the implementation in Node.js, not closing a MessagePort does not
//   prevent anything from exiting. It's best to close MessagePorts manually
//   but the current implementation does not require it.
//
// Because of these differences we do not currently run the full suite of web
// platform tests against our implementation -- we know most of them will fail
// since most of them depend on the ability to transfer MessagePorts or depend
// on the mechanisms we do not implement. And yes, we know that this means that
// if we need stricter compliance with the spec in the future we will likely
// need to introduce a compat flag.
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
    closeImpl();
  }

  // MessagePort instances cannot be created directly.
  // Use `new MessageChannel()`
  static jsg::Ref<MessagePort> constructor() = delete;

  void postMessage(jsg::Lock& js,
      jsg::Optional<jsg::JsRef<jsg::JsValue>> data = kj::none,
      jsg::Optional<TransferListOrOptions> options = kj::none);
  void closeImpl();
  void close(jsg::Lock& js);
  void start(jsg::Lock& js);

  // The onmessage event handler IDL attribute
  // (see EventTarget::setEventHandlerAttribute).
  kj::Maybe<jsg::JsValue> getOnMessage(jsg::Lock& js);
  void setOnMessage(
      jsg::Lock& js, jsg::Optional<kj::OneOf<EventTarget::HandlerFunction, jsg::JsValue>> handler);

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
  static void entangle(jsg::Lock& js, jsg::Ref<MessagePort>& port1, jsg::Ref<MessagePort>& port2);

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

  // Two ports are entangled when they weakly reference each other.
  // Keep in mind that this is a weak reference! So if one of the
  // ports gets GC'd the other will will also end up being closed.
  // To keep them both alive, maintain strong references to both
  // ports!
  kj::Maybe<jsg::WeakRef<MessagePort>> other;

  void listenerCountChanged(jsg::Lock& js, kj::StringPtr type, size_t count) override;

  void visitForGc(jsg::GcVisitor& visitor) {
    KJ_IF_SOME(pending, state.tryGet<Pending>()) {
      visitor.visitAll(pending);
    }
  }
};

// MessageChannel is simple enough... create a couple of MessagePorts
// and entangle those so that they will exchange messages with each
// other.
class MessageChannel final: public jsg::Object {
 public:
  MessageChannel(jsg::Ref<MessagePort> port1, jsg::Ref<MessagePort> port2)
      : port1(kj::mv(port1)),
        port2(kj::mv(port2)) {}

  static jsg::Ref<MessageChannel> constructor(jsg::Lock& js);

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

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(port1, port2);
  }
};

// Module that exposes MessageChannel and MessagePort for internal use by
// built-in modules like node:worker_threads without requiring the global
// expose_global_message_channel compat flag.
class MessageChannelModule final: public jsg::Object {
 public:
  MessageChannelModule() = default;
  MessageChannelModule(jsg::Lock&, const jsg::Url&) {}

  JSG_RESOURCE_TYPE(MessageChannelModule) {
    JSG_NESTED_TYPE(MessageChannel);
    JSG_NESTED_TYPE(MessagePort);
  }
};

template <class Registry>
void registerMessageChannelModule(Registry& registry, auto featureFlags) {
  registry.template addBuiltinModule<MessageChannelModule>(
      "cloudflare-internal:messagechannel", workerd::jsg::ModuleRegistry::Type::INTERNAL);
}

template <typename TypeWrapper>
kj::Own<jsg::modules::ModuleBundle> getInternalMessageChannelModuleBundle(auto featureFlags) {
  jsg::modules::ModuleBundle::BuiltinBuilder builder(
      jsg::modules::ModuleBundle::BuiltinBuilder::Type::BUILTIN_ONLY);
  static const auto kSpecifier = "cloudflare-internal:messagechannel"_url;
  builder.addObject<MessageChannelModule, TypeWrapper>(kSpecifier);
  return builder.finish();
}

}  // namespace workerd::api

#define EW_MESSAGECHANNEL_ISOLATE_TYPES                                                            \
  api::MessagePort, api::MessageChannel, api::MessagePort::PostMessageOptions,                     \
      api::MessageChannelModule
