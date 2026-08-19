#include "messagechannel.h"

#include "blob.h"
#include "events.h"

#include <workerd/io/features.h>
#include <workerd/jsg/ser.h>

namespace workerd::api {
MessagePort::MessagePort(): state(Pending()) {}

// Tracks 'message' listener registrations — both addEventListener() listeners and the
// onmessage attribute's trampoline — to transition the port between states: the first
// listener starts the port (delivering any queued messages), and removing the last one
// returns it to pending (queueing messages again). Counting every listener is technically
// not spec compliant (per spec only assigning onmessage enables the message queue), but it
// is what Node.js does.
void MessagePort::listenerCountChanged(jsg::Lock& js, kj::StringPtr type, size_t count) {
  if (type != "message"_kj) {
    return;
  }
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(pending, Pending) {
      if (count > 0) {
        start(js);
      }
    }
    KJ_CASE_ONEOF(started, Started) {
      if (count == 0) {
        state = Pending();
      }
    }
    KJ_CASE_ONEOF(_, Closed) {
      // Closed is terminal: listener changes never restart the port.
    }
  }
}

void MessagePort::dispatchMessage(jsg::Lock& js, const jsg::JsValue& value) {
  auto policy = effectiveExceptionPolicy(js, DispatchExceptionPolicy::REPORT);
  auto result = dispatchEventImpl(js,
      js.alloc<MessageEvent>(js, value, kj::String(), JSG_THIS, kj::none, Trusted::YES), policy);
  KJ_IF_SOME(exception, result.firstException) {
    // A 'message' listener threw. Its exception was reported (and the remaining 'message'
    // listeners still ran); additionally surface it as a 'messageerror' event on this port,
    // carrying the exception as the event's data. The 'messageerror' dispatch itself is
    // report-only: a throwing 'messageerror' listener triggers no further reaction.
    dispatchEventImpl(js,
        js.alloc<MessageEvent>(js, kj::str("messageerror"), exception.addRef(js), kj::String(),
            JSG_THIS, kj::none, Trusted::YES),
        policy);
  }
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
void MessagePort::entangle(
    jsg::Lock& js, jsg::Ref<MessagePort>& port1, jsg::Ref<MessagePort>& port2) {
  port1->other = port2.getWeakRef(js);
  port2->other = port1.getWeakRef(js);
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

  // Per the spec, postMessage() serializes the message (StructuredSerializeWithTransfer) before it
  // is routed to the peer. This means a non-serializable value (e.g. a Blob or an RpcTarget) must
  // throw a DataCloneError even if the peer port has been closed or garbage-collected. We must not
  // skip this validation just because there is no live recipient — otherwise the observable
  // behavior of postMessage() depends on whether the peer happens to still be reachable, which is
  // non-deterministic (the peer is held only via a weak ref and may be collected at any time).
  jsg::Serializer ser(js);
  KJ_IF_SOME(d, data) {
    ser.write(js, d.getHandle(js));
  } else {
    ser.write(js, js.undefined());
  }

  auto released = ser.release();
  JSG_REQUIRE(released.sharedArrayBuffers.size() == 0, TypeError,
      "SharedArrayBuffer is unsupported with MessagePort");

  // If the port is closed or the peer has been collected, just drop the (already-validated)
  // message.
  KJ_IF_SOME(o, other) {
    KJ_IF_SOME(ref, o.tryAddRef(js)) {
      // Deserialize the message into a JsValue and deliver it to the peer.
      jsg::Deserializer deserializer(js, released);
      auto clonedData = deserializer.readValue(js);
      ref->deliver(js, clonedData);
    }
  }
}

void MessagePort::closeImpl() {
  // Any pending messages will be dropped on the floor, except for those that were
  // already scheduled for delivery in the `start()` or `deliver()` methods.
  if (state.is<Closed>()) return;
  state = Closed{};
  KJ_IF_SOME(o, other) {
    // Use of tryGet here rather than tryAddRef is intentional. closeImpl
    // is called from the destructor, where we may or may not have the
    // isolate lock. Materializing a strong reference to the other port
    // requires the isolate lock. The other = kj::none line below will
    // ensure that the jsg::WeakRef is cleaned up under lock either
    // immediately or eventually.
    KJ_IF_SOME(ref, o.tryGet()) {
      ref.closeImpl();
    }
    other = kj::none;
  }
}

void MessagePort::close(jsg::Lock& js) {
  static constexpr kj::StringPtr name = "close"_kj;
  if (state.is<Closed>()) return;
  state = Closed{};
  KJ_IF_SOME(o, other) {
    KJ_IF_SOME(ref, o.tryAddRef(js)) {
      ref->close(js);
    }
    other = kj::none;
  }
  auto closeEvent = js.alloc<Event>(name, Event::Init{}, Trusted::YES);
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
  return getEventHandlerAttribute(js, "message"_kj);
}

void MessagePort::setOnMessage(
    jsg::Lock& js, jsg::Optional<kj::OneOf<EventTarget::HandlerFunction, jsg::JsValue>> handler) {
  // The attribute's trampoline registration and removal flow through
  // listenerCountChanged(), which starts and stops the port; nothing else to do here.
  setEventHandlerAttribute(js, "message"_kj, kj::mv(handler));
}

jsg::Ref<MessageChannel> MessageChannel::constructor(jsg::Lock& js) {
  auto port1 = js.alloc<MessagePort>();
  auto port2 = js.alloc<MessagePort>();
  MessagePort::entangle(js, port1, port2);
  return js.alloc<MessageChannel>(kj::mv(port1), kj::mv(port2));
}

}  // namespace workerd::api
