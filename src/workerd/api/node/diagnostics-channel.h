#pragma once

#include "async-hooks.h"
#include <workerd/jsg/jsg.h>
#include <kj/map.h>
#include <kj/table.h>

namespace workerd::api::node {

class Channel : public jsg::Object {
public:
  using MessageCallback = jsg::Function<void(jsg::JsValue, jsg::Name)>;
  using TransformCallback = jsg::Function<jsg::JsValue(jsg::JsValue)>;

  static jsg::JsValue identityTransform(jsg::Lock& js, jsg::JsValue value);

  Channel(jsg::Name name);

  bool hasSubscribers();
  void publish(jsg::Lock& js, jsg::JsValue message);
  void subscribe(jsg::Lock& js, jsg::Identified<MessageCallback> callback);
  void unsubscribe(jsg::Lock& js, jsg::Identified<MessageCallback> callback);
  void bindStore(jsg::Lock& js, jsg::Ref<AsyncLocalStorage> als,
                 jsg::Optional<TransformCallback> maybeTransform);
  void unbindStore(jsg::Lock& js, jsg::Ref<AsyncLocalStorage> als);
  jsg::JsValue runStores(
      jsg::Lock& js,
      jsg::JsValue message,
      jsg::Function<jsg::JsValue(jsg::Arguments<jsg::JsRef<jsg::JsValue>>)> callback,
      jsg::Optional<jsg::JsValue> maybeReceiver,
      jsg::Arguments<jsg::JsRef<jsg::JsValue>> args);

  JSG_RESOURCE_TYPE(Channel) {
    JSG_METHOD(hasSubscribers);
    JSG_METHOD(publish);
    JSG_METHOD(subscribe);
    JSG_METHOD(unsubscribe);
    JSG_METHOD(bindStore);
    JSG_METHOD(unbindStore);
    JSG_METHOD(runStores);
  }

  const jsg::Name& getName() const;

private:

  struct StoreEntry {
    kj::Own<jsg::AsyncContextFrame::StorageKey> key;
    TransformCallback transform;
  };

  struct StoreCallbacks {
    auto& keyForRow(StoreEntry& row) const {
      return *row.key;
    }

    bool matches(const StoreEntry& a, jsg::AsyncContextFrame::StorageKey& key) const {
      return a.key.get() == &key;
    }

    uint hashCode(jsg::AsyncContextFrame::StorageKey& key) const {
      return key.hashCode();
    }
  };

  struct SubscriberKey {
    int hash;
    jsg::JsRef<jsg::JsObject> ref;
    int hashCode() const { return hash; };
    // TODO(cleanup): Later, jsg::Identified should be updated to use jsg::JsRef
    // rather than V8Ref/HashableV8Ref.
    SubscriberKey(jsg::Lock& js, jsg::Identified<MessageCallback>& callback)
        : SubscriberKey(js, jsg::JsObject(callback.identity.getHandle(js))) {}
    SubscriberKey(jsg::Lock& js, const jsg::JsObject& key)
        : hash(key.hashCode()),
          ref(js, key) {}
    bool operator==(const SubscriberKey& other) const {
      return hash == other.hash;
    }
    void visitForGc(jsg::GcVisitor& visitor) {
      visitor.visit(ref);
    }
  };

  jsg::Name name;
  kj::HashMap<SubscriberKey, MessageCallback> subscribers;
  kj::Table<StoreEntry, kj::HashIndex<StoreCallbacks>> stores;

  void visitForGc(jsg::GcVisitor& visitor);
};

class DiagnosticsChannelModule : public jsg::Object {
public:
  bool hasSubscribers(jsg::Lock& js, jsg::Name name);
  jsg::Ref<Channel> channel(jsg::Lock& js, jsg::Name name);
  void subscribe(jsg::Lock& js, jsg::Name name, jsg::Identified<Channel::MessageCallback> callback);
  void unsubscribe(jsg::Lock& js, jsg::Name name, jsg::Identified<Channel::MessageCallback> callback);
  // TODO: Support tracing channels

  JSG_RESOURCE_TYPE(DiagnosticsChannelModule) {
    JSG_METHOD(hasSubscribers);
    JSG_METHOD(channel);
    JSG_METHOD(subscribe);
    JSG_METHOD(unsubscribe);
    JSG_NESTED_TYPE(Channel);
  }

  kj::Maybe<Channel&> tryGetChannel(jsg::Lock& js, jsg::Name& name);

private:
  kj::HashMap<kj::String, jsg::Ref<Channel>> channels;

  void visitForGc(jsg::GcVisitor& visitor);
};

#define EW_NODE_DIAGNOSTICCHANNEL_ISOLATE_TYPES        \
    api::node::Channel,                                \
    api::node::DiagnosticsChannelModule

}  // namespace workerd::api::node
