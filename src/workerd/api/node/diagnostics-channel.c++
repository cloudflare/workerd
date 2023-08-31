#include "diagnostics-channel.h"
#include <workerd/io/io-context.h>
#include <workerd/io/trace.h>
#include <workerd/jsg/ser.h>

namespace workerd::api::node {

jsg::JsValue Channel::identityTransform(jsg::Lock& js, jsg::JsValue value) {
  return value;
}

Channel::Channel(jsg::Name name) : name(kj::mv(name)) {}

const jsg::Name& Channel::getName() const { return name; }

bool Channel::hasSubscribers() {
  return subscribers.size() != 0;
}

void Channel::publish(jsg::Lock& js, jsg::JsValue message) {
  js.withinHandleScope([&] {
    for (auto& sub : subscribers) {
      sub.value(js, message, name.clone(js));
    }

    auto& context = IoContext::current();
    KJ_IF_MAYBE(tracer, context.getWorkerTracer()) {
      jsg::Serializer ser(js, jsg::Serializer::Options {
        .omitHeader = false,
      });
      ser.write(js, message);
      auto tmp = ser.release();
      JSG_REQUIRE(tmp.sharedArrayBuffers.size() == 0 &&
                  tmp.transferedArrayBuffers.size() == 0, Error,
                  "Diagnostic events cannot be published with SharedArrayBuffer or "
                  "transferred ArrayBuffer instances");
      tracer->addDiagnosticChannelEvent(context.now(), name.toString(js), kj::mv(tmp.data));
    }
  });
}

void Channel::subscribe(jsg::Lock& js, jsg::Identified<MessageCallback> callback) {
  subscribers.upsert(SubscriberKey(js, callback),
                     kj::mv(callback.unwrapped),
                     [&](auto&, auto&&) {});
}

void Channel::unsubscribe(jsg::Lock& js, jsg::Identified<MessageCallback> callback) {
  subscribers.erase(SubscriberKey(js, callback));
}

void Channel::bindStore(jsg::Lock& js, jsg::Ref<AsyncLocalStorage> als,
                        jsg::Optional<TransformCallback> maybeTransform) {
  auto key = als->getKey();
  KJ_IF_MAYBE(entry, stores.find(*key)) {
    KJ_IF_MAYBE(transform, maybeTransform) {
      entry->transform = kj::mv(*transform);
    } else {
      entry->transform = [](jsg::Lock& js, jsg::JsValue value) {
        return identityTransform(js, value);
      };
    }
    return;
  }

  KJ_IF_MAYBE(transform, maybeTransform) {
    stores.insert({
      .key = kj::mv(key),
      .transform = kj::mv(*transform)
    });
  } else {
    stores.insert({
      .key = kj::mv(key),
      .transform = [](jsg::Lock& js, jsg::JsValue value) {
        return identityTransform(js, value);
      }
    });
  }
}

void Channel::unbindStore(jsg::Lock& js, jsg::Ref<AsyncLocalStorage> als) {
  auto key = als->getKey();
  stores.eraseMatch(*key);
}

jsg::JsValue Channel::runStores(
    jsg::Lock& js,
    jsg::JsValue message,
    jsg::Function<jsg::JsValue(jsg::Arguments<jsg::JsRef<jsg::JsValue>>)> callback,
    jsg::Optional<jsg::JsValue> maybeReceiver,
    jsg::Arguments<jsg::JsRef<jsg::JsValue>> args) {
  auto storageScopes = KJ_MAP(store, stores) {
    return kj::heap<jsg::AsyncContextFrame::StorageScope>(js, *store.key,
        store.transform(js, message));
  };

  publish(js, message);

  jsg::JsValue receiver = js.global();
  KJ_IF_MAYBE(val, maybeReceiver) {
    receiver = *val;
  }
  callback.setReceiver(js, receiver);
  return callback(js, kj::mv(args));
}

void Channel::visitForGc(jsg::GcVisitor& visitor) {
  for (auto& sub : subscribers) {
    visitor.visit(sub.key, sub.value);
  }
  for (auto& store: stores) {
    visitor.visit(store.transform);
  }
}

bool DiagnosticsChannelModule::hasSubscribers(jsg::Lock& js, jsg::Name name) {
  return tryGetChannel(js, name).map([&](Channel& channel) {
    return channel.hasSubscribers();
  }).orDefault(false);
}

void DiagnosticsChannelModule::subscribe(jsg::Lock& js,
                                         jsg::Name name,
                                         jsg::Identified<Channel::MessageCallback> callback) {
  channel(js, kj::mv(name))->subscribe(js, kj::mv(callback));
}

void DiagnosticsChannelModule::unsubscribe(jsg::Lock& js,
                                           jsg::Name name,
                                           jsg::Identified<Channel::MessageCallback> callback) {
  KJ_IF_MAYBE(channel, tryGetChannel(js, name)) {
    channel->unsubscribe(js, kj::mv(callback));
  }
}

jsg::Ref<Channel> DiagnosticsChannelModule::channel(jsg::Lock& js, jsg::Name channel) {
  kj::String name = channel.toString(js);
  return channels.findOrCreate(name, [&, channel=kj::mv(channel)]() mutable ->
      kj::HashMap<kj::String, jsg::Ref<Channel>>::Entry{
    return { kj::mv(name), jsg::alloc<Channel>(kj::mv(channel)) };
  }).addRef();
}

kj::Maybe<Channel&> DiagnosticsChannelModule::tryGetChannel(jsg::Lock& js, jsg::Name& name) {
  return channels.find(name.toString(js)).map([](jsg::Ref<Channel>& channel) -> Channel& {
    return *channel;
  });
}

void DiagnosticsChannelModule::visitForGc(jsg::GcVisitor& visitor) {
  for (auto& channel : channels) {
    visitor.visit(channel.value);
  }
}

}  // namespace workerd::api::node
