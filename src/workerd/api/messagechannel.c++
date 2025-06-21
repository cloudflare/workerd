#include "messagechannel.h"

#include "global-scope.h"
#include "worker-rpc.h"

#include <workerd/io/worker.h>
#include <workerd/jsg/ser.h>

#include <capnp/message.h>

namespace workerd::api {
jsg::Ref<MessagePort::MessageEvent> MessagePort::MessageEvent::New(jsg::Lock& js,
    jsg::JsRef<jsg::JsValue> data,
    kj::String lastEventId,
    jsg::Ref<MessagePort> source,
    kj::Array<jsg::Ref<MessagePort>> ports) {
  return jsg::alloc<MessagePort::MessageEvent>(
      kj::str("message"), kj::mv(data), kj::mv(lastEventId), kj::mv(source), kj::mv(ports));
}

jsg::Ref<MessagePort::MessageEvent> MessagePort::MessageEvent::NewError(jsg::Lock& js,
    jsg::JsRef<jsg::JsValue> data,
    kj::String lastEventId,
    jsg::Ref<MessagePort> source,
    kj::Array<jsg::Ref<MessagePort>> ports) {
  return jsg::alloc<MessagePort::MessageEvent>(
      kj::str("messageerror"), kj::mv(data), kj::mv(lastEventId), kj::mv(source), kj::mv(ports));
}

MessagePort::MessagePort(IoContext& ioContext): ioContext(ioContext), state(Pending()) {
  // We set a callback on the underlying EventTarget to be notified when
  // a listener for the message event is added or removed. When there
  // are no listeners, we move back to the Pending state, otherwise we
  // will switch to the Started state if necessary.
  setEventListenerCallback([&](jsg::Lock& js, kj::StringPtr name, size_t count) {
    if (name == "message"_kj) {
      KJ_SWITCH_ONEOF(state) {
        KJ_CASE_ONEOF(pending, Pending) {
          // If we are in the pending state, start the port if we have listeners.
          // This is technically not spec compliant, but it is what Node.js
          // supports. Specifically, adding a new message listener using the
          // addEventListener method is *technically* not supposed to start
          // the port but we're going to do what Node.js does.
          if (count > 0 || onmessageValue != kj::none) {
            start(js);
          }
        }
        KJ_CASE_ONEOF(started, Started) {
          // If we are in the started state, stop the port if there are no listeners.
          if (count == 0 && onmessageValue == kj::none) {
            state = Pending();
          }
        }
        KJ_CASE_ONEOF(_, Closed) {
          // Nothing to do. We're already closed so we don't care.
        }
      }
    }
  });
}

// Deliver the message to the "message" or "messageerror" event on this port.
void MessagePort::deliver(
    jsg::Lock& js, kj::Own<rpc::JsValue::Reader> message, jsg::Ref<MessagePort> port) {
  auto event = js.tryCatch([&] {
    jsg::Deserializer deserializer(js, message->getV8Serialized());
    return MessageEvent::New(
        js, jsg::JsRef(js, deserializer.readValue(js)), kj::str(), port->addRef(), {});
  }, [&](jsg::Value exception) {
    return MessageEvent::NewError(
        js, jsg::JsRef(js, jsg::JsValue(exception.getHandle(js))), kj::str(), port->addRef(), {});
  });

  // If any of the message/messageerror event handlers throw,
  // capture the error and pass it to reportError instead of
  // propagating up.
  js.tryCatch([&] { port->dispatchEventImpl(js, kj::mv(event)); }, [&](jsg::Value exception) {
    auto context = js.v8Context();
    auto& global =
        jsg::extractInternalPointer<ServiceWorkerGlobalScope, true>(context, context->Global());
    global.reportError(js, jsg::JsValue(exception.getHandle(js)));
  });
}

// Deliver the message to all the jsrpc remotes we have
kj::Promise<void> MessagePort::sendToRpc(kj::Own<rpc::JsValue::Reader> message) {
  KJ_IF_SOME(outputLocks, ioContext.waitForOutputLocksIfNecessary()) {
    co_await outputLocks;
  }
  kj::Vector<kj::Promise<void>> promises;
  for (rpc::JsMessagePort::Client cap: rpcClients) {
    auto req = cap.callRequest();
    req.setData(*message);
    // TODO(message-port): Removing the port when dropped?
    promises.add(req.send().ignoreResult());
  }
  co_await kj::joinPromises(promises.releaseAsArray());
}

// Deliver the message to this port, buffering if necessary if the port
// has not been started. Buffered messages will be delivered when the
// port is started later.
void MessagePort::deliverMessage(jsg::Lock& js, rpc::JsValue::Reader value) {
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(pending, Pending) {
      // We have not yet started the port so buffer to message.
      pending.add(capnp::clone(value));
    }
    KJ_CASE_ONEOF(started, Started) {
      ioContext.addTask(sendToRpc(capnp::clone(value)));
      deliver(js, capnp::clone(value), addRef());
    }
    KJ_CASE_ONEOF(_, Closed) {
      // Nothing to do in this case. Drop the message on the floor.
    }
  }
}

// Binds two ports to each other such that messages posted to one
// are delivered on the other.
void MessagePort::entangle(MessagePort& port1, MessagePort& port2) {
  port1.other = port2.addRef();
  port2.other = port1.addRef();
}

// Post a message to the entangled port.
void MessagePort::postMessage(jsg::Lock& js,
    jsg::Optional<jsg::JsRef<jsg::JsValue>> data,
    jsg::Optional<TransferListOrOptions> options) {

  KJ_IF_SOME(opt, options) {
    // We don't currently support transfer lists, even for local
    // same-isolate delivery.
    // TODO(conform): Implement transfer later?
    KJ_SWITCH_ONEOF(opt) {
      KJ_CASE_ONEOF(list, TransferList) {
        JSG_REQUIRE(list.size() == 0, Error, "Transfer list is not supported");
      }
      KJ_CASE_ONEOF(opts, PostMessageOptions) {
        KJ_IF_SOME(list, opts.transfer) {
          JSG_REQUIRE(list.size() == 0, Error, "Transfer list is not supported");
        }
      }
    }
  }

  KJ_IF_SOME(o, other) {
    // TODO(message-port): Set up the external handler to support more types.
    jsg::Serializer ser(js);
    KJ_IF_SOME(d, data) {
      ser.write(js, d.getHandle(js));
    }
    auto released = ser.release();
    JSG_REQUIRE(released.sharedArrayBuffers.size() == 0, Error, "SharedArrayBuffer is unsupported");

    capnp::MallocMessageBuilder builder;
    rpc::JsValue::Builder val = builder.initRoot<rpc::JsValue>();
    val.setV8Serialized(kj::mv(released.data));
    o->deliverMessage(js, val.asReader());
  }
}

// Should close this port, the entangle port, and any known rpc clients.
void MessagePort::close() {
  state = Closed{};
  rpcClients.clear();
  KJ_IF_SOME(o, other) {
    auto closing = kj::mv(o);
    other = kj::none;
    closing->close();
  }
}

// Start delivering messages on this port. Any messages that are
// buffered will be drained immediately.
void MessagePort::start(jsg::Lock& js) {
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(pending, Pending) {
      auto list = kj::mv(pending);
      state = Started{};
      for (auto& item: list) {
        ioContext.addTask(sendToRpc(capnp::clone(*item)));
        // Local delivery
        deliver(js, kj::mv(item), addRef());
      }
    }
    KJ_CASE_ONEOF(_, Started) {
      // Nothing to do in this case. We are already started!
    }
    KJ_CASE_ONEOF(_, Closed) {
      // Nothing to do in this case. Can't start after closing.
    }
  }
}

kj::Maybe<jsg::JsValue> MessagePort::getOnMessage(jsg::Lock& js) {
  return onmessageValue.map(
      [&](jsg::JsRef<jsg::JsValue>& ref) -> jsg::JsValue { return ref.getHandle(js); });
}

void MessagePort::setOnMessage(jsg::Lock& js, jsg::JsValue value) {
  if (!value.isObject() && !value.isFunction()) {
    onmessageValue = kj::none;
    // If we have no handlers and no onmessage ...
    if (getHandlerCount("message"_kj) == 0 && onmessageValue == kj::none) {
      // ...Put the port back into a pending state where messages
      // will be enqueued until another listener is attached.
      state = Pending();
    }
  } else {
    onmessageValue = jsg::JsRef<jsg::JsValue>(js, value);
    start(js);
  }
}

namespace {
// The jsrpc handler that receives messages posted from the remote and
// delivers them to the local port.
class JsMessagePortImpl final: public rpc::JsMessagePort::Server {
 public:
  JsMessagePortImpl(IoContext& ctx, jsg::Ref<MessagePort> port)
      : port(kj::mv(port)),
        weakIoContext(ctx.getWeakRef()) {}
  ~JsMessagePortImpl() {
    port->close();
  }

  kj::Promise<void> call(CallContext context) override {
    IoContext& ctx = JSG_REQUIRE_NONNULL(weakIoContext->tryGet(), Error,
        "The destination object for this message port no longer exists.");

    KJ_IF_SOME(other, port->getOther()) {
      // We can only dispatch messages under the isolate lock, so acquire
      // that here then deliver...
      auto lock = co_await ctx.getWorker()->takeAsyncLockWithoutRequest(nullptr);
      ctx.getWorker()->runInLockScope(lock, [&](auto& lock) {
        JSG_WITHIN_CONTEXT_SCOPE(lock, lock.getContext(), [&](auto& js) {
          auto params = context.getParams();
          other.deliverMessage(js, params.getData());
        });
      });
    }

    context.initResults();
  }

 private:
  jsg::Ref<MessagePort> port;
  kj::Own<IoContext::WeakRef> weakIoContext;
};
}  // namespace

void MessagePort::serialize(jsg::Lock& js, jsg::Serializer& serializer) {
  auto& handler = JSG_REQUIRE_NONNULL(serializer.getExternalHandler(), DOMDataCloneError,
      "MessagePort can only be serialized for RPC.");
  auto externalHandler = dynamic_cast<RpcSerializerExternalHander*>(&handler);
  JSG_REQUIRE(
      externalHandler != nullptr, DOMDataCloneError, "MessagePort can only be serialized for RPC.");

  // Don't send a MessagePort that has been closed already.
  JSG_REQUIRE(!state.is<Closed>(), DOMDataCloneError, "MessagePort is closed");

  // What needs to happen here?
  // Every time a port is serialized, a two capabilities need to be created...
  // One locally that allows sending message to the remote side,
  // And one remotely that allows sending messages to this side.
  // These capabilities are similar to streams but instead of sending
  // bytes we're sending the rpc::JsValue.
  auto& ioContext = IoContext::current();

  auto streamCap =
      externalHandler->writeStream([&](rpc::JsValue::External::Builder builder) mutable {
    auto params = builder.initMessagePort();
    params.setOut(kj::heap<JsMessagePortImpl>(ioContext, JSG_THIS));
  });

  rpcClients.add(kj::mv(streamCap).castAs<rpc::JsMessagePort>());
  start(js);
}

jsg::Ref<MessagePort> MessagePort::deserialize(
    jsg::Lock& js, rpc::SerializationTag tag, jsg::Deserializer& deserializer) {

  auto& handler = JSG_REQUIRE_NONNULL(deserializer.getExternalHandler(), DOMDataCloneError,
      "MessagePort can only be serialized for RPC.");
  auto externalHandler = dynamic_cast<RpcDeserializerExternalHander*>(&handler);
  JSG_REQUIRE(
      externalHandler != nullptr, DOMDataCloneError, "MessagePort can only be serialized for RPC.");

  auto reader = externalHandler->read();

  KJ_REQUIRE(reader.isMessagePort(), "external table slot type does't match serialization tag");
  auto other = reader.getMessagePort();
  auto otherHandler = other.getOut();

  auto& ioContext = IoContext::current();
  auto port1 = jsg::alloc<MessagePort>(ioContext);
  auto port2 = jsg::alloc<MessagePort>(ioContext);
  MessagePort::entangle(*port1, *port2);

  port1->rpcClients.add(kj::mv(otherHandler));

  port1->start(js);
  externalHandler->setLastStream(kj::heap<JsMessagePortImpl>(ioContext, kj::mv(port1)));

  return kj::mv(port2);
}

jsg::Ref<MessageChannel> MessageChannel::constructor() {
  auto& ioContext = IoContext::current();
  auto port1 = jsg::alloc<MessagePort>(ioContext);
  auto port2 = jsg::alloc<MessagePort>(ioContext);
  MessagePort::entangle(*port1, *port2);
  return jsg::alloc<MessageChannel>(kj::mv(port1), kj::mv(port2));
}

}  // namespace workerd::api
