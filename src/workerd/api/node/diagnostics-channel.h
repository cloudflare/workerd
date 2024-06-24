#pragma once

#include "async-hooks.h"
#include <workerd/jsg/jsg.h>
#include <kj/map.h>
#include <kj/table.h>

namespace workerd::api::node {

class Channel : public jsg::Object {
public:
  using MessageCallback = jsg::Function<void(jsg::Value, jsg::Name)>;
  using TransformCallback = jsg::Function<jsg::Value(jsg::Value)>;

  static jsg::Value identityTransform(jsg::Lock& js, jsg::Value value);

  Channel(jsg::Name name);

  bool hasSubscribers();
  void publish(jsg::Lock& js, jsg::Value message);
  void subscribe(jsg::Lock& js, jsg::Identified<MessageCallback> callback);
  void unsubscribe(jsg::Lock& js, jsg::Identified<MessageCallback> callback);
  void bindStore(jsg::Lock& js, jsg::Ref<AsyncLocalStorage> als,
                 jsg::Optional<TransformCallback> maybeTransform);
  void unbindStore(jsg::Lock& js, jsg::Ref<AsyncLocalStorage> als);
  v8::Local<v8::Value> runStores(
      jsg::Lock& js,
      jsg::Value message,
      jsg::Function<v8::Local<v8::Value>(jsg::Arguments<jsg::Value>)> callback,
      jsg::Optional<v8::Local<v8::Value>> maybeReceiver,
      jsg::Arguments<jsg::Value> args);

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

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const;

private:

  struct StoreEntry {
    kj::Own<jsg::AsyncContextFrame::StorageKey> key;
    TransformCallback transform;
    JSG_MEMORY_INFO(StoreEntry) {
      tracker.trackField("transform", transform);
    }
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

  jsg::Name name;
  kj::HashMap<jsg::HashableV8Ref<v8::Object>, MessageCallback> subscribers;
  kj::Table<StoreEntry, kj::HashIndex<StoreCallbacks>> stores;

  void visitForGc(jsg::GcVisitor& visitor);
};

class DiagnosticsChannelModule : public jsg::Object {
public:
  DiagnosticsChannelModule() = default;
  DiagnosticsChannelModule(jsg::Lock&, const jsg::Url&) {}

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

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const;

private:
  kj::HashMap<kj::String, jsg::Ref<Channel>> channels;

  void visitForGc(jsg::GcVisitor& visitor);
};

#define EW_NODE_DIAGNOSTICCHANNEL_ISOLATE_TYPES        \
    api::node::Channel,                                \
    api::node::DiagnosticsChannelModule

}  // namespace workerd::api::node
