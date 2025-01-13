#include "message-channel.h"

#include <workerd/api/events.h>
#include <workerd/jsg/jsg.h>

namespace workerd::api {

MessagePort::~MessagePort() noexcept(false) {
  // Technically we should dispatch a close event whenever this object
  // is garbage collected. Unfortunately, we cannot guarantee that the destructor
  // will always have the isolate lock.
  // Let's keep in mind that the destructor might be running during v8 GC
  // in which you shouldn't execute any javascript at all.
  // dispatchEvent(js, Event::constructor(kj::str("close"), kj::none));
}

jsg::Ref<MessagePort> MessagePort::constructor() {
  return jsg::alloc<MessagePort>();
}

void MessagePort::disentangle(jsg::Lock &js) {
  KJ_IF_SOME(e, entangledWith) {
    // Fire an event named close at otherPort.
    e->dispatchEvent(js, CloseEvent::constructor());
    e->entangledWith = kj::none;
  }
  entangledWith = kj::none;
}

void MessagePort::entangle(jsg::Lock &js, jsg::Ref<MessagePort> port) {
  disentangle(js);
  entangledWith = port.addRef();
  port->entangledWith = JSG_THIS;
}

void MessagePort::postMessage(jsg::Lock &js,
    jsg::Value message,
    kj::OneOf<kj::Maybe<StructuredSerializeOptions>, kj::Array<jsg::Value>> options) {}

void MessagePort::start(jsg::Lock &js) {
  // The start() method steps are to enable this's port message queue, if it is not already enabled.
  if (messageQueue == kj::none) {
    messageQueue = kj::Vector<Message>();
  }
}

void MessagePort::stop(jsg::Lock &js) {}

void MessagePort::close(jsg::Lock &js) {
  // Set this's [[Detached]] internal slot value to true.
  detached = true;
  // If this is entangled, disentangle it.
  disentangle(js);
  // The close event will be fired even if the port is not explicitly closed.
  dispatchEvent(js, CloseEvent::constructor());
}

MessageChannel::MessageChannel(jsg::Lock &js)
    : port1(MessagePort::constructor()),
      port2(MessagePort::constructor()) {}

jsg::Ref<MessageChannel> MessageChannel::constructor(jsg::Lock &js) {
  return jsg::alloc<MessageChannel>(js);
}

}  // namespace workerd::api
