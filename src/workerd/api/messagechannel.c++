#include "messagechannel.h"

#include "events.h"

#include <workerd/io/worker.h>
#include <workerd/jsg/ser.h>
#include <workerd/util/weak-refs.h>

namespace workerd::api {
MessagePort::MessagePort()
    : weakThis(kj::refcounted<WeakRef<MessagePort>>(kj::Badge<MessagePort>{}, *this)),
      state(Pending()) {
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

void MessagePort::dispatchMessage(jsg::Lock& js, const jsg::JsValue& value) {
  js.tryCatch([&] {
    auto message = js.alloc<MessageEvent>(js, kj::str("message"), value, kj::String(), JSG_THIS);
    dispatchEventImpl(js, kj::mv(message));
  }, [&](jsg::Value exception) {
    // There was an error dispatching the message event.
    // We will dispatch a messageerror event instead.
    auto message = js.alloc<MessageEvent>(
        js, kj::str("message"), jsg::JsValue(exception.getHandle(js)), kj::String(), JSG_THIS);
    dispatchEventImpl(js, kj::mv(message));
    // Now, if this dispatchEventImpl throws, we just blow up. Don't try to catch it.
  });
}

// Deliver the message to this port, buffering if necessary if the port
// has not been started. Buffered messages will be delivered when the
// port is started later.
void MessagePort::deliver(jsg::Lock& js, const jsg::JsValue& value) {
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(pending, Pending) {
      // We have not yet started the port so buffer the message.
      // It will be delivered when the port is started.
      // We don't know how many messages will be buffered, if any,
      // so we avoid reserving space in the array.
      pending.add(jsg::JsRef(js, value));
    }
    KJ_CASE_ONEOF(started, Started) {
      js.resolvedPromise().then(
          js, [self = JSG_THIS, value = jsg::JsRef(js, value)](jsg::Lock& js) mutable {
        self->dispatchMessage(js, value.getHandle(js));
      });
    }
    KJ_CASE_ONEOF(_, Closed) {
      // Nothing to do in this case. Drop the message on the floor.
    }
  }
}

// Binds two ports to each other such that messages posted to one
// are delivered on the other.
void MessagePort::entangle(MessagePort& port1, MessagePort& port2) {
  port1.other = port2.addWeakRef();
  port2.other = port1.addWeakRef();
}

// Post a message to the entangled port.
void MessagePort::postMessage(jsg::Lock& js,
    jsg::Optional<jsg::JsRef<jsg::JsValue>> data,
    jsg::Optional<TransferListOrOptions> options) {

  // We don't currently support transfer lists, even for local
  // same-isolate delivery.
  // TODO(conform): Implement transfer later?
  bool hasTransfer = false;
  KJ_SWITCH_ONEOF(kj::mv(options).orDefault(PostMessageOptions{})) {
    KJ_CASE_ONEOF(list, TransferList) {
      hasTransfer = list.size() > 0;
    }
    KJ_CASE_ONEOF(opts, PostMessageOptions) {
      KJ_IF_SOME(list, opts.transfer) {
        hasTransfer = list.size() > 0;
      }
    }
  }
  JSG_REQUIRE(!hasTransfer, Error, "Transfer list is not supported");

  // If the port is closed, other will be kj::none and we will just drop the message.
  other->runIfAlive([&](MessagePort& o) {
    jsg::Serializer ser(js);

    KJ_IF_SOME(d, data) {
      ser.write(js, d.getHandle(js));
    } else {
      ser.write(js, js.undefined());
    }

    auto released = ser.release();
    JSG_REQUIRE(released.sharedArrayBuffers.size() == 0, TypeError,
        "SharedArrayBuffer is unsupported with MessagePort");

    // Now, deserialize the message into a JsValue
    jsg::Deserializer deserializer(js, released);
    auto clonedData = deserializer.readValue(js);
    o.deliver(js, clonedData);
  });
}

void MessagePort::closeImpl() {
  // Any pending messages will be dropped on the floor, except for those that were
  // already scheduled for delivery in the `start()` or `deliver()` methods.
  if (state.is<Closed>()) return;
  state = Closed{};
  weakThis->invalidate();
  other->runIfAlive([&](MessagePort& o) { o.closeImpl(); });
}

void MessagePort::close(jsg::Lock& js) {
  if (state.is<Closed>()) return;
  state = Closed{};
  weakThis->invalidate();
  other->runIfAlive([&](MessagePort& o) { o.close(js); });
  auto closeEvent = js.alloc<Event>(kj::str("close"), Event::Init{}, true);
  dispatchEventImpl(js, kj::mv(closeEvent));
}

// Start delivering messages on this port. Any messages that are
// buffered will be drained immediately.
void MessagePort::start(jsg::Lock& js) {
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(pending, Pending) {
      auto list = kj::mv(pending);
      state = Started{};
      // We're going to dispatch the messages using a microtask so that the actual
      // delivery is deferred to match Node.js' behavior as close as possible.
      js.resolvedPromise().then(js, [list = kj::mv(list), self = JSG_THIS](jsg::Lock& js) mutable {
        for (auto& item: list) {
          self->dispatchMessage(js, item.getHandle(js));
        }
      });
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

jsg::Ref<MessageChannel> MessageChannel::constructor(jsg::Lock& js) {
  auto port1 = js.alloc<MessagePort>();
  auto port2 = js.alloc<MessagePort>();
  MessagePort::entangle(*port1, *port2);
  return js.alloc<MessageChannel>(kj::mv(port1), kj::mv(port2));
}

}  // namespace workerd::api
