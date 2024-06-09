#pragma once

#include <workerd/jsg/jsg.h>

namespace workerd::api {

class Event;
class EventTarget;
class AbortSignal;
class AbortController;
class Subscriber;
class Observable;
using VoidFunction = jsg::Function<void()>;
using SubscribeCallback = jsg::Function<void(jsg::Ref<Subscriber>)>;
using SubscriptionObserverCallback = jsg::Function<void(jsg::JsValue)>;

using HandlerFunction = jsg::Function<jsg::Optional<jsg::Value>(jsg::Ref<Event>)>;

class Subscriber final: public jsg::Object {
public:
  struct InternalObserver {
    SubscriptionObserverCallback next;
    SubscriptionObserverCallback error;
    VoidFunction complete;
    InternalObserver(SubscriptionObserverCallback next,
                     SubscriptionObserverCallback error,
                     VoidFunction complete)
        : next(kj::mv(next)),
          error(kj::mv(error)),
          complete(kj::mv(complete)) {}
  };

  Subscriber(jsg::Lock& js,
             Observable& observable,
             kj::Own<InternalObserver> inner,
             kj::Maybe<jsg::Ref<AbortSignal>> signalFromOptions,
             const jsg::TypeHandler<HandlerFunction>& handler);

  void next(jsg::Lock& js, jsg::JsValue value);
  void error(jsg::Lock& js, jsg::JsValue error);
  void complete(jsg::Lock& js);
  void addTeardown(jsg::Lock& js, VoidFunction teardown);

  bool getActive();
  jsg::Ref<AbortSignal> getSignal();

  JSG_RESOURCE_TYPE(Subscriber) {
    JSG_METHOD(next);
    JSG_METHOD(error);
    JSG_METHOD(complete);
    JSG_METHOD(addTeardown);
    JSG_READONLY_PROTOTYPE_PROPERTY(active, getActive);
    JSG_READONLY_PROTOTYPE_PROPERTY(signal, getSignal);
  }

  void close();
  void setupTeardown(jsg::Lock& js);

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const;

  Observable& getObservable();

private:
  Observable& observable_;
  kj::Maybe<kj::Own<InternalObserver>> inner_;
  jsg::Ref<AbortController> completeOrErrorController_;
  jsg::Ref<AbortSignal> signal_;
  kj::Vector<VoidFunction> teardowns_;
  kj::Maybe<kj::Own<void>> teardownHandler_;

  void visitForGc(jsg::GcVisitor& visitor);
};

struct SubscriptionObserver {
  jsg::Optional<SubscriptionObserverCallback> next;
  jsg::Optional<SubscriptionObserverCallback> error;
  jsg::Optional<VoidFunction> complete;

  JSG_STRUCT(next, error, complete);
};

using ObserverUnion = kj::OneOf<SubscriptionObserverCallback,
                                SubscriptionObserver>;
using ObserverUnionImpl = kj::OneOf<SubscriptionObserverCallback,
                                SubscriptionObserver,
                                kj::Own<Subscriber::InternalObserver>>;

struct SubscribeOptions {
  jsg::Optional<jsg::Ref<AbortSignal>> signal;
  JSG_STRUCT(signal);
};

using Predicate = jsg::Function<bool(jsg::JsValue, uint32_t index)>;
using Reducer = jsg::Function<jsg::JsValue(jsg::JsValue accumulator,
                                           jsg::JsValue currentValue)>;
using Mapper = jsg::Function<jsg::JsValue(jsg::JsValue element, uint32_t index)>;
using Visitor = jsg::Function<void(jsg::JsValue element, uint32_t index)>;

class Observable final: public jsg::Object {
public:
  Observable(
      jsg::Lock& js,
      SubscribeCallback callback,
      const jsg::TypeHandler<HandlerFunction>& handler,
      const jsg::TypeHandler<jsg::Ref<Observable>>& observableHandler,
      const jsg::TypeHandler<jsg::Promise<jsg::JsRef<jsg::JsValue>>>& promiseHandler,
      const jsg::TypeHandler<jsg::AsyncGenerator<jsg::JsRef<jsg::JsValue>>>& asyncGeneratorHandler);
  static jsg::Ref<Observable> constructor(
      jsg::Lock& js,
      SubscribeCallback callback,
      const jsg::TypeHandler<HandlerFunction>& handler,
      const jsg::TypeHandler<jsg::Ref<Observable>>& observableHandler,
      const jsg::TypeHandler<jsg::Promise<jsg::JsRef<jsg::JsValue>>>& promiseHandler,
      const jsg::TypeHandler<jsg::AsyncGenerator<jsg::JsRef<jsg::JsValue>>>& asyncGeneratorHandler);

  void subscribe(jsg::Lock& js, jsg::Optional<ObserverUnion> observer,
                 jsg::Optional<SubscribeOptions> options,
                 const jsg::TypeHandler<HandlerFunction>& handler);
  void subscribeImpl(jsg::Lock& js, jsg::Optional<ObserverUnionImpl> observer,
                     jsg::Optional<SubscribeOptions> options,
                     const jsg::TypeHandler<HandlerFunction>& handler);

  static jsg::Ref<Observable> from(
      jsg::Lock& js,
      jsg::JsValue value,
      const jsg::TypeHandler<HandlerFunction>& handler,
      const jsg::TypeHandler<jsg::Ref<Observable>>& observableHandler,
      const jsg::TypeHandler<jsg::Promise<jsg::JsRef<jsg::JsValue>>>& promiseHandler,
      const jsg::TypeHandler<jsg::AsyncGenerator<jsg::JsRef<jsg::JsValue>>>& asyncGeneratorHandler);

  jsg::Ref<Observable> takeUntil(jsg::Lock& js, jsg::JsValue notifier);
  jsg::Ref<Observable> map(jsg::Lock& js, Mapper mapper);
  jsg::Ref<Observable> filter(jsg::Lock& js, Predicate predicate);
  jsg::Ref<Observable> take(jsg::Lock& js, uint32_t amount);
  jsg::Ref<Observable> drop(jsg::Lock& js, uint32_t amount);
  jsg::Ref<Observable> flatMap(jsg::Lock& js, Mapper mapper);
  jsg::Ref<Observable> switchMap(jsg::Lock& js, Mapper mapper);
  jsg::Ref<Observable> finally(jsg::Lock& js, VoidFunction callback);

  jsg::Promise<kj::Array<jsg::JsRef<jsg::JsValue>>> toArray(
      jsg::Lock& js,
      jsg::Optional<SubscribeOptions> options);
  jsg::Promise<void> forEach(
      jsg::Lock& js,
      Visitor callback,
      jsg::Optional<SubscribeOptions> options);
  jsg::Promise<bool> every(
      jsg::Lock& js,
      Predicate predicate,
      jsg::Optional<SubscribeOptions> options);
  jsg::Promise<jsg::JsRef<jsg::JsValue>> first(
      jsg::Lock& js,
      jsg::Optional<SubscribeOptions> options);
  jsg::Promise<jsg::JsRef<jsg::JsValue>> last(
      jsg::Lock& js,
      jsg::Optional<SubscribeOptions> options);
  jsg::Promise<jsg::JsRef<jsg::JsValue>> find(
      jsg::Lock& js,
      Predicate predicate,
      jsg::Optional<SubscribeOptions> options);
  jsg::Promise<bool> some(
      jsg::Lock& js,
      Predicate predicate,
      jsg::Optional<SubscribeOptions> options);
  jsg::Promise<jsg::JsRef<jsg::JsValue>> reduce(
      jsg::Lock& js,
      Reducer reducer,
      jsg::Optional<jsg::JsValue> initialValue,
      jsg::Optional<SubscribeOptions> options);

  JSG_RESOURCE_TYPE(Observable) {
    JSG_STATIC_METHOD(from);

    JSG_METHOD(subscribe);
    JSG_METHOD(takeUntil);
    JSG_METHOD(map);
    JSG_METHOD(filter);
    JSG_METHOD(take);
    JSG_METHOD(drop);
    JSG_METHOD(flatMap);
    JSG_METHOD(switchMap);
    JSG_METHOD(finally);
    JSG_METHOD(toArray);
    JSG_METHOD(forEach);
    JSG_METHOD(every);
    JSG_METHOD(first);
    JSG_METHOD(last);
    JSG_METHOD(find);
    JSG_METHOD(some);
    JSG_METHOD(reduce);
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const;

  void setNativeHandler(kj::Own<void> handler) {
    nativeHandler = kj::mv(handler);
  }

private:
  SubscribeCallback callback_;
  const jsg::TypeHandler<HandlerFunction>& handler_;
  const jsg::TypeHandler<jsg::Ref<Observable>>& observableHandler_;
  const jsg::TypeHandler<jsg::Promise<jsg::JsRef<jsg::JsValue>>>& promiseHandler_;
  const jsg::TypeHandler<jsg::AsyncGenerator<jsg::JsRef<jsg::JsValue>>>& asyncGeneratorHandler_;
  kj::Maybe<kj::Own<void>> nativeHandler = kj::none;

  void visitForGc(jsg::GcVisitor& visitor);
};

struct ObservableEventListenerOptions {
  jsg::Optional<bool> capture;
  jsg::Optional<bool> passive;
  JSG_STRUCT(capture, passive);
};

#define EW_OBSERVABLE_ISOLATE_TYPES      \
    api::Observable,                     \
    api::Subscriber,                     \
    api::SubscriptionObserver,           \
    api::SubscribeOptions,               \
    api::ObservableEventListenerOptions

}  // namespace workerd::api
