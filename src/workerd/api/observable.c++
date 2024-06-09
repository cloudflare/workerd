#include "observable.h"
#include "basics.h"
#include <kj/vector.h>
#include <deque>

namespace workerd::api {

namespace {
jsg::Ref<AbortSignal> getSubscriberSignal(jsg::Lock& js,
    jsg::Ref<AbortController>& completeOrErrorController,
    kj::Maybe<jsg::Ref<AbortSignal>> signalFromOptions,
    const jsg::TypeHandler<HandlerFunction>& handler) {
  kj::Vector<jsg::Ref<AbortSignal>> signals;
  signals.add(completeOrErrorController->getSignal());
  KJ_IF_SOME(signal, signalFromOptions) {
    signals.add(signal.addRef());
  }
  return AbortSignal::any(js, signals.releaseAsArray(), handler);
}

struct IndexHolder {
  uint32_t idx = 0;
};

struct FlatmapState : public kj::Refcounted {
  uint32_t idx = 0;
  bool outerSubscriptionHasCompleted = false;
  bool activeInnerSubscription = false;
  std::deque<jsg::JsRef<jsg::JsValue>> queue;
  Mapper mapper;
  FlatmapState(Mapper mapper) : mapper(kj::mv(mapper)) {}
};

void runFlatMap(
    jsg::Lock& js, const jsg::JsValue& value,
    jsg::Ref<Subscriber> subscriber,
    kj::Rc<FlatmapState> state,
    const jsg::TypeHandler<HandlerFunction>& handler,
    const jsg::TypeHandler<jsg::Ref<Observable>>& observableHandler,
    const jsg::TypeHandler<jsg::Promise<jsg::JsRef<jsg::JsValue>>>& promiseHandler,
    const jsg::TypeHandler<jsg::AsyncGenerator<jsg::JsRef<jsg::JsValue>>>& asyncGeneratorHandler) {
  js.tryCatch([&] {
    auto mappedValue = state->mapper(js, value, state->idx);
    state->idx++;
    auto innerObservable = Observable::from(js, mappedValue, handler, observableHandler,
                                            promiseHandler, asyncGeneratorHandler);
    innerObservable->subscribeImpl(js,
        kj::heap<Subscriber::InternalObserver>(
          JSG_VISITABLE_LAMBDA(
              (subscriber=subscriber.addRef()),
              (subscriber),
              (jsg::Lock& js, jsg::JsValue value) {
            js.tryCatch([&] {
              subscriber->next(js, value);
            }, [&](jsg::Value exception) {
              subscriber->error(js, jsg::JsValue(exception.getHandle(js)));
            });
          }),
          JSG_VISITABLE_LAMBDA(
              (subscriber=subscriber.addRef()),
              (subscriber),
              (jsg::Lock& js, jsg::JsValue error) {
            subscriber->error(js, error);
          }),
          JSG_VISITABLE_LAMBDA(
              (subscriber=subscriber.addRef(), state=state.addRef(),
               &handler, &observableHandler, &promiseHandler,
               &asyncGeneratorHandler),
              (subscriber),
              (jsg::Lock& js) {
            js.tryCatch([&] {
              if (state->queue.size() > 0) {
                auto nextValue = state->queue.front().addRef(js);
                state->queue.pop_front();
                runFlatMap(js, nextValue.getHandle(js), subscriber.addRef(),
                          state.addRef(), handler, observableHandler, promiseHandler,
                          asyncGeneratorHandler);
              } else {
                state->activeInnerSubscription = false;
                if (state->outerSubscriptionHasCompleted) {
                  subscriber->complete(js);
                }
              }
            }, [&](jsg::Value exception) {
              subscriber->error(js, jsg::JsValue(exception.getHandle(js)));
            });
          })),
        SubscribeOptions {
          .signal = subscriber->getSignal(),
        }, handler);
  }, [&](jsg::Value exception) {
    subscriber->error(js, jsg::JsValue(exception.getHandle(js)));
  });
}

struct SwitchmapState : public kj::Refcounted {
  uint32_t idx = 0;
  bool outerSubscriptionHasCompleted = false;
  kj::Maybe<jsg::Ref<AbortController>> activeInnerAbortController;
  Mapper mapper;
  SwitchmapState(Mapper mapper) : mapper(kj::mv(mapper)) {}
};

void runSwitchMap(
    jsg::Lock& js, const jsg::JsValue& value,
    jsg::Ref<Subscriber> subscriber,
    kj::Rc<SwitchmapState> state,
    const jsg::TypeHandler<HandlerFunction>& handler,
    const jsg::TypeHandler<jsg::Ref<Observable>>& observableHandler,
    const jsg::TypeHandler<jsg::Promise<jsg::JsRef<jsg::JsValue>>>& promiseHandler,
    const jsg::TypeHandler<jsg::AsyncGenerator<jsg::JsRef<jsg::JsValue>>>& asyncGeneratorHandler) {
  js.tryCatch([&] {
    auto mappedResult = state->mapper(js, value, state->idx);
    state->idx++;
    auto innerObservable = Observable::from(js, mappedResult, handler, observableHandler,
                                            promiseHandler, asyncGeneratorHandler);
    innerObservable->subscribeImpl(js,
        kj::heap<Subscriber::InternalObserver>(
          JSG_VISITABLE_LAMBDA(
              (subscriber=subscriber.addRef()),
              (subscriber),
              (jsg::Lock& js, jsg::JsValue value) {
            js.tryCatch([&] {
              subscriber->next(js, value);
            }, [&](jsg::Value exception) {
              subscriber->error(js, jsg::JsValue(exception.getHandle(js)));
            });
          }),
          JSG_VISITABLE_LAMBDA(
              (subscriber=subscriber.addRef()),
              (subscriber),
              (jsg::Lock& js, jsg::JsValue error) {
            subscriber->error(js, error);
          }),
          JSG_VISITABLE_LAMBDA(
              (subscriber=subscriber.addRef(), state=state.addRef()),
              (subscriber),
              (jsg::Lock& js) {
            if (state->outerSubscriptionHasCompleted) {
              subscriber->complete(js);
            } else {
              state->activeInnerAbortController = kj::none;
            }
          })),
        SubscribeOptions {
          .signal = subscriber->getSignal(),
        }, handler);
  }, [&](jsg::Value exception) {
    subscriber->error(js, jsg::JsValue(exception.getHandle(js)));
  });
}
}  // namespace

Subscriber::Subscriber(jsg::Lock& js,
                       Observable& observable,
                       kj::Own<InternalObserver> inner,
                       kj::Maybe<jsg::Ref<AbortSignal>> signalFromOptions,
                       const jsg::TypeHandler<HandlerFunction>& handler)
    : observable_(observable),
      inner_(kj::mv(inner)),
      completeOrErrorController_(jsg::alloc<AbortController>()),
      signal_(getSubscriberSignal(js, completeOrErrorController_,
                                  kj::mv(signalFromOptions), handler)) {}

void Subscriber::next(jsg::Lock& js, jsg::JsValue value) {
  KJ_IF_SOME(inner, inner_) {
    inner->next(js, value);
  }
}

void Subscriber::error(jsg::Lock& js, jsg::JsValue error) {
  KJ_IF_SOME(inner, inner_) {
    inner->error(js, error);
  }
}

void Subscriber::complete(jsg::Lock& js) {
  KJ_IF_SOME(inner, inner_) {
    inner->complete(js);
  }
}

void Subscriber::addTeardown(jsg::Lock& js, VoidFunction teardown) {
  teardowns_.add(kj::mv(teardown));
}

bool Subscriber::getActive() {
  return inner_ != kj::none;
}

jsg::Ref<AbortSignal> Subscriber::getSignal() {
  return signal_.addRef();
}

void Subscriber::close() {
  inner_ = kj::none;
}

void Subscriber::setupTeardown(jsg::Lock& js) {
  teardownHandler_ = signal_->newNativeHandler(js, kj::str("abort"),
      [this](jsg::Lock& js, jsg::Ref<Event> event) {
    close();
    // Run teardowns in reverse insertion order
    js.tryCatch([&] {
      if (!teardowns_.empty()) {
        for (auto i = teardowns_.size() - 1; i >= 0; --i) {
          teardowns_[i](js);
        }
      }
    }, [&](jsg::Value exception) {
      error(js, jsg::JsValue(exception.getHandle(js)));
    });
  }, true);
}

void Subscriber::visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
  KJ_IF_SOME(inner, inner_) {
    tracker.trackField("next", inner->next);
    tracker.trackField("error", inner->error);
    tracker.trackField("complete", inner->complete);
  }
  tracker.trackField("signal", signal_);
  for (auto& teardown : teardowns_) {
    tracker.trackField("teardown", teardown);
  }
}

void Subscriber::visitForGc(jsg::GcVisitor& visitor) {
  KJ_IF_SOME(inner, inner_) {
    visitor.visit(inner->next, inner->error, inner->complete);
  }
  visitor.visit(signal_);
  visitor.visitAll(teardowns_);
}

Observable& Subscriber::getObservable() { return observable_; }

Observable::Observable(
    jsg::Lock& js,
    SubscribeCallback callback,
    const jsg::TypeHandler<HandlerFunction>& handler,
    const jsg::TypeHandler<jsg::Ref<Observable>>& observableHandler,
    const jsg::TypeHandler<jsg::Promise<jsg::JsRef<jsg::JsValue>>>& promiseHandler,
    const jsg::TypeHandler<jsg::AsyncGenerator<jsg::JsRef<jsg::JsValue>>>& asyncGeneratorHandler)
    : callback_(kj::mv(callback)),
      handler_(handler),
      observableHandler_(observableHandler),
      promiseHandler_(promiseHandler),
      asyncGeneratorHandler_(asyncGeneratorHandler) {}

jsg::Ref<Observable> Observable::constructor(
    jsg::Lock& js,
    SubscribeCallback callback,
    const jsg::TypeHandler<HandlerFunction>& handler,
    const jsg::TypeHandler<jsg::Ref<Observable>>& observableHandler,
    const jsg::TypeHandler<jsg::Promise<jsg::JsRef<jsg::JsValue>>>& promiseHandler,
    const jsg::TypeHandler<jsg::AsyncGenerator<jsg::JsRef<jsg::JsValue>>>& asyncGeneratorHandler) {
  return jsg::alloc<Observable>(js, kj::mv(callback),
                                handler,
                                observableHandler,
                                promiseHandler,
                                asyncGeneratorHandler);
}

void Observable::subscribe(jsg::Lock& js, jsg::Optional<ObserverUnion> observer,
                           jsg::Optional<SubscribeOptions> options,
                           const jsg::TypeHandler<HandlerFunction>& handler) {
  subscribeImpl(js, kj::mv(observer), kj::mv(options), handler);
}

void Observable::subscribeImpl(jsg::Lock& js, jsg::Optional<ObserverUnionImpl> observer,
                           jsg::Optional<SubscribeOptions> options,
                               const jsg::TypeHandler<HandlerFunction>& handler) {
  auto internalObserver = ([&] {
    KJ_IF_SOME(o, observer) {
      KJ_SWITCH_ONEOF(o) {
        KJ_CASE_ONEOF(callback, SubscriptionObserverCallback) {
          return kj::heap<Subscriber::InternalObserver>(
            JSG_VISITABLE_LAMBDA(
              (callback=kj::mv(callback)),
              (callback),
              (jsg::Lock& js, jsg::JsValue value) mutable {
            return js.tryCatch([&] {
              callback(js, value);
            }, [&](jsg::Value exception) {
              js.reportError(jsg::JsValue(exception.getHandle(js)));
            });
          }),
          [](jsg::Lock& js, jsg::JsValue error) {
            js.reportError(error);
          },
          [](jsg::Lock& js) {});
        }
        KJ_CASE_ONEOF(observer, SubscriptionObserver) {
          return kj::heap<Subscriber::InternalObserver>(
            observer.next.map([](SubscriptionObserverCallback& callback) {
                return kj::mv(callback);
              }).orDefault([](jsg::Lock&, jsg::JsValue) {}),
            observer.error.map([](SubscriptionObserverCallback& callback) {
                return kj::mv(callback);
              }).orDefault([](jsg::Lock& js, jsg::JsValue error) {
                js.reportError(error);
              }),
            observer.complete.map([](VoidFunction& callback) {
                return kj::mv(callback);
              }).orDefault([](jsg::Lock&) {}));
        }
        KJ_CASE_ONEOF(internal, kj::Own<Subscriber::InternalObserver>) {
          return kj::mv(internal);
        }
      }
    }
    return kj::heap<Subscriber::InternalObserver>(
      [](jsg::Lock&, jsg::JsValue) {},
      [](jsg::Lock& js, jsg::JsValue error) {
        js.reportError(error);
      },
      [](jsg::Lock&) {});
  })();

  SubscribeOptions opts = kj::mv(options).orDefault({});
  auto subscriber = jsg::alloc<Subscriber>(js, *this, kj::mv(internalObserver),
                                           kj::mv(opts.signal), handler);

  if (subscriber->getSignal()->getAborted()) {
    subscriber->close();
  } else {
    subscriber->setupTeardown(js);
  }

  js.tryCatch([&] {
    callback_(js, subscriber.addRef());
  }, [&](jsg::Value exception) {
    subscriber->error(js, jsg::JsValue(exception.getHandle(js)));
  });
}

jsg::Ref<Observable> Observable::takeUntil(jsg::Lock& js, jsg::JsValue notifier) {
  return jsg::alloc<Observable>(js,
      JSG_VISITABLE_LAMBDA(
          (this,
           sourceObservable=JSG_THIS,
           notifier=Observable::from(js, notifier, handler_,
                                     observableHandler_, promiseHandler_,
                                     asyncGeneratorHandler_)),
          (notifier),
          (jsg::Lock&js, jsg::Ref<Subscriber> subscriber) {
        notifier->subscribeImpl(js,
            kj::heap<Subscriber::InternalObserver>(
                JSG_VISITABLE_LAMBDA(
                  (subscriber=subscriber.addRef()),
                  (subscriber),
                  (jsg::Lock& js, jsg::JsValue) {
                  subscriber->complete(js);
                }),
                JSG_VISITABLE_LAMBDA(
                  (subscriber=subscriber.addRef()),
                  (subscriber),
                  (jsg::Lock& js, jsg::JsValue) {
                  subscriber->complete(js);
                }),
                [](jsg::Lock&) {}),
            SubscribeOptions { .signal = subscriber->getSignal(), },
            handler_);
        if (!subscriber->getActive()) return;
        sourceObservable->subscribeImpl(js,
            kj::heap<Subscriber::InternalObserver>(
                JSG_VISITABLE_LAMBDA(
                    (subscriber=subscriber.addRef()),
                    (subscriber),
                    (jsg::Lock& js, jsg::JsValue value) {
                  subscriber->next(js, value);
                }),
                JSG_VISITABLE_LAMBDA(
                    (subscriber=subscriber.addRef()),
                    (subscriber),
                    (jsg::Lock& js, jsg::JsValue error) {
                  subscriber->error(js, error);
                }),
                JSG_VISITABLE_LAMBDA(
                    (subscriber=subscriber.addRef()),
                    (subscriber),
                    (jsg::Lock& js) {
                  subscriber->complete(js);
                })),
            SubscribeOptions { .signal = subscriber->getSignal(), },
            handler_);
      }), handler_, observableHandler_, promiseHandler_, asyncGeneratorHandler_);
}

jsg::Ref<Observable> Observable::map(jsg::Lock& js, Mapper mapper) {
  return jsg::alloc<Observable>(js,
      JSG_VISITABLE_LAMBDA(
          (this, sourceObserverable=JSG_THIS, mapper=kj::mv(mapper)),
          (sourceObserverable),
          (jsg::Lock& js, jsg::Ref<Subscriber> subscriber) {
    sourceObserverable->subscribeImpl(js, kj::heap<Subscriber::InternalObserver>(
      JSG_VISITABLE_LAMBDA(
          (subscriber=subscriber.addRef(),
           mapper=kj::mv(mapper),
           holder=kj::heap<IndexHolder>()),
          (subscriber, mapper),
          (jsg::Lock& js, jsg::JsValue value) {
        auto mappedValue = js.tryCatch([&]() -> kj::Maybe<jsg::JsValue> {
          return mapper(js, value, holder->idx);
        }, [&](jsg::Value exception) -> kj::Maybe<jsg::JsValue> {
          subscriber->error(js, jsg::JsValue(exception.getHandle(js)));
          return kj::none;
        });
        KJ_IF_SOME(value, mappedValue) {
          holder->idx++;
          subscriber->next(js, value);
        } else {}
      }),
      JSG_VISITABLE_LAMBDA(
          (subscriber=subscriber.addRef()),
          (subscriber),
          (auto& js, auto error) { subscriber->error(js, error); }),
      JSG_VISITABLE_LAMBDA(
          (subscriber=subscriber.addRef()),
          (subscriber),
          (jsg::Lock& js) { subscriber->complete(js); })),
          SubscribeOptions { .signal = subscriber->getSignal(),
    }, handler_);
  }), handler_, observableHandler_, promiseHandler_, asyncGeneratorHandler_);
}

jsg::Ref<Observable> Observable::filter(jsg::Lock& js, Predicate predicate) {
  return jsg::alloc<Observable>(js,
      JSG_VISITABLE_LAMBDA(
          (this, sourceObserverable=JSG_THIS, predicate=kj::mv(predicate)),
          (sourceObserverable),
          (jsg::Lock& js, jsg::Ref<Subscriber> subscriber) {
    sourceObserverable->subscribeImpl(js, kj::heap<Subscriber::InternalObserver>(
      JSG_VISITABLE_LAMBDA(
          (subscriber=subscriber.addRef(),
           predicate=kj::mv(predicate),
           holder=kj::heap<IndexHolder>()),
          (subscriber, predicate),
          (jsg::Lock& js, jsg::JsValue value) {
        auto matches = js.tryCatch([&]() -> kj::Maybe<bool> {
          return predicate(js, value, holder->idx);
        }, [&](jsg::Value exception) -> kj::Maybe<bool> {
          subscriber->error(js, jsg::JsValue(exception.getHandle(js)));
          return kj::none;
        });
        KJ_IF_SOME(m, matches) {
          if (m) {
            holder->idx++;
            subscriber->next(js, value);
          }
        } else {}
      }),
      JSG_VISITABLE_LAMBDA(
          (subscriber=subscriber.addRef()),
          (subscriber),
          (auto& js, auto error) { subscriber->error(js, error); }),
      JSG_VISITABLE_LAMBDA(
          (subscriber=subscriber.addRef()),
          (subscriber),
          (jsg::Lock& js) { subscriber->complete(js); })),
          SubscribeOptions { .signal = subscriber->getSignal(),
    }, handler_);
  }), handler_, observableHandler_, promiseHandler_, asyncGeneratorHandler_);
}

jsg::Ref<Observable> Observable::take(jsg::Lock& js, uint32_t amount) {
  auto holder = kj::heap<IndexHolder>();
  holder->idx = amount;
  return jsg::alloc<Observable>(js,
      JSG_VISITABLE_LAMBDA(
          (this, sourceObserverable=JSG_THIS, holder=kj::mv(holder)),
          (sourceObserverable),
          (jsg::Lock& js, jsg::Ref<Subscriber> subscriber) {
    if (holder->idx == 0) {
      subscriber->complete(js);
      return;
    }
    sourceObserverable->subscribeImpl(js, kj::heap<Subscriber::InternalObserver>(
      JSG_VISITABLE_LAMBDA(
          (subscriber=subscriber.addRef(),
           holder=kj::mv(holder)),
          (subscriber),
          (jsg::Lock& js, jsg::JsValue value) {
        js.tryCatch([&] {
          subscriber->next(js, value);
          if (--holder->idx == 0) {
            subscriber->complete(js);
          }
        }, [&](jsg::Value exception) {
          subscriber->error(js, jsg::JsValue(exception.getHandle(js)));
        });
      }),
      JSG_VISITABLE_LAMBDA(
          (subscriber=subscriber.addRef()),
          (subscriber),
          (auto& js, auto error) { subscriber->error(js, error); }),
      JSG_VISITABLE_LAMBDA(
          (subscriber=subscriber.addRef()),
          (subscriber),
          (jsg::Lock& js) { subscriber->complete(js); })),
          SubscribeOptions { .signal = subscriber->getSignal(),
    }, handler_);
  }), handler_, observableHandler_, promiseHandler_, asyncGeneratorHandler_);
}

jsg::Ref<Observable> Observable::drop(jsg::Lock& js, uint32_t amount) {
  auto holder = kj::heap<IndexHolder>();
  holder->idx = amount;
  return jsg::alloc<Observable>(js,
      JSG_VISITABLE_LAMBDA(
          (this, sourceObserverable=JSG_THIS, holder=kj::mv(holder)),
          (sourceObserverable),
          (jsg::Lock& js, jsg::Ref<Subscriber> subscriber) {
    sourceObserverable->subscribeImpl(js, kj::heap<Subscriber::InternalObserver>(
      JSG_VISITABLE_LAMBDA(
          (subscriber=subscriber.addRef(),
           holder=kj::mv(holder)),
          (subscriber),
          (jsg::Lock& js, jsg::JsValue value) {
        if (holder->idx > 0) {
          holder->idx--;
          return;
        }
        js.tryCatch([&] {
          subscriber->next(js, value);
        }, [&](jsg::Value exception) {
          subscriber->error(js, jsg::JsValue(exception.getHandle(js)));
        });
      }),
      JSG_VISITABLE_LAMBDA(
          (subscriber=subscriber.addRef()),
          (subscriber),
          (auto& js, auto error) { subscriber->error(js, error); }),
      JSG_VISITABLE_LAMBDA(
          (subscriber=subscriber.addRef()),
          (subscriber),
          (jsg::Lock& js) { subscriber->complete(js); })),
          SubscribeOptions { .signal = subscriber->getSignal(),
    }, handler_);
  }), handler_, observableHandler_, promiseHandler_, asyncGeneratorHandler_);
}

jsg::Ref<Observable> Observable::flatMap(jsg::Lock& js, Mapper mapper) {
  return jsg::alloc<Observable>(js,
      JSG_VISITABLE_LAMBDA(
          (this, sourceObserverable=JSG_THIS, mapper=kj::mv(mapper)),
          (sourceObserverable, mapper),
          (jsg::Lock& js, jsg::Ref<Subscriber> subscriber) {
    auto state = kj::rc<FlatmapState>(kj::mv(mapper));
    sourceObserverable->subscribeImpl(js, kj::heap<Subscriber::InternalObserver>(
      JSG_VISITABLE_LAMBDA(
          (this, subscriber=subscriber.addRef(),
           mapper=kj::mv(mapper),
           state=state.addRef()),
          (subscriber),
          (jsg::Lock& js, jsg::JsValue value) {
        if (state->activeInnerSubscription) {
          state->queue.push_back(jsg::JsRef(js, value));
        } else {
          state->activeInnerSubscription = true;
          runFlatMap(js, value, subscriber.addRef(), state.addRef(), handler_,
                     observableHandler_, promiseHandler_,
                     asyncGeneratorHandler_);
        }
      }),
      JSG_VISITABLE_LAMBDA(
          (subscriber=subscriber.addRef()),
          (subscriber),
          (auto& js, auto error) { subscriber->error(js, error); }),
      JSG_VISITABLE_LAMBDA(
          (subscriber=subscriber.addRef(), state=state.addRef()),
          (subscriber),
          (jsg::Lock& js) {
        state->outerSubscriptionHasCompleted = true;
        if (!state->activeInnerSubscription && state->queue.size() == 0) {
          subscriber->complete(js);
        }
      })), SubscribeOptions { .signal = subscriber->getSignal(),
    }, handler_);
  }), handler_, observableHandler_, promiseHandler_, asyncGeneratorHandler_);
}

jsg::Ref<Observable> Observable::switchMap(jsg::Lock& js, Mapper mapper) {
  return jsg::alloc<Observable>(js,
      JSG_VISITABLE_LAMBDA(
          (this, sourceObserverable=JSG_THIS, mapper=kj::mv(mapper)),
          (sourceObserverable, mapper),
          (jsg::Lock& js, jsg::Ref<Subscriber> subscriber) {
    auto state = kj::rc<SwitchmapState>(kj::mv(mapper));
    sourceObserverable->subscribeImpl(js, kj::heap<Subscriber::InternalObserver>(
      JSG_VISITABLE_LAMBDA(
          (this, subscriber=subscriber.addRef(),
           mapper=kj::mv(mapper),
           state=state.addRef()),
          (subscriber),
          (jsg::Lock& js, jsg::JsValue value) {
        KJ_IF_SOME(ac, state->activeInnerAbortController) {
          ac->abort(js, kj::none);
        } else {}
        state->activeInnerAbortController = jsg::alloc<AbortController>();
        runSwitchMap(js, value, subscriber.addRef(), state.addRef(), handler_,
                     observableHandler_, promiseHandler_,
                     asyncGeneratorHandler_);
      }),
      JSG_VISITABLE_LAMBDA(
          (subscriber=subscriber.addRef()),
          (subscriber),
          (auto& js, auto error) { subscriber->error(js, error); }),
      JSG_VISITABLE_LAMBDA(
          (subscriber=subscriber.addRef(), state=state.addRef()),
          (subscriber),
          (jsg::Lock& js) {
        state->outerSubscriptionHasCompleted = true;
        if (state->activeInnerAbortController == kj::none) {
          subscriber->complete(js);
        }
      })), SubscribeOptions { .signal = subscriber->getSignal(),
    }, handler_);
  }), handler_, observableHandler_, promiseHandler_, asyncGeneratorHandler_);
}

jsg::Ref<Observable> Observable::finally(jsg::Lock& js, VoidFunction callback) {
  // TODO(conform): The Observable spec does not yet provide a definition for this.
  JSG_FAIL_REQUIRE(Error, "Implementation not yet defined");
}

jsg::Promise<kj::Array<jsg::JsRef<jsg::JsValue>>> Observable::toArray(
    jsg::Lock& js,
    jsg::Optional<SubscribeOptions> options) {
  auto paf = js.newPromiseAndResolver<kj::Array<jsg::JsRef<jsg::JsValue>>>();
  struct ToArrayState: public kj::Refcounted {
    jsg::Promise<kj::Array<jsg::JsRef<jsg::JsValue>>>::Resolver resolver;
    kj::Maybe<kj::Own<void>> handler;
    kj::Vector<jsg::JsRef<jsg::JsValue>> values;
    ToArrayState(jsg::Promise<kj::Array<jsg::JsRef<jsg::JsValue>>>::Resolver resolver)
        : resolver(kj::mv(resolver)) {}
  };
  auto state = kj::refcounted<ToArrayState>(kj::mv(paf.resolver));
  KJ_IF_SOME(opt, options) {
    KJ_IF_SOME(signal, opt.signal) {
      if (signal->getAborted()) {
        state->resolver.reject(js, signal->getReason(js));
        return kj::mv(paf.promise);
      }
      state->handler = signal->newNativeHandler(js, kj::str("abort"),
          [&state=*state, &signal=*signal](jsg::Lock& js, auto event) mutable {
        state.resolver.reject(js, signal.getReason(js));
      }, true).attach(signal.addRef());
    }
  }
  subscribeImpl(js, kj::heap<Subscriber::InternalObserver>(
    [state=kj::addRef(*state)](jsg::Lock& js, jsg::JsValue value) mutable {
      state->values.add(jsg::JsRef(js, value));
    },
    [state=kj::addRef(*state)](jsg::Lock& js, jsg::JsValue error) mutable {
      state->resolver.reject(js, error);
    },
    [state=kj::addRef(*state)](jsg::Lock& js) mutable {
      state->resolver.resolve(js, state->values.releaseAsArray());
    }), kj::mv(options), handler_);

  return kj::mv(paf.promise);
}

jsg::Promise<void> Observable::forEach(
    jsg::Lock& js,
    Visitor callback,
    jsg::Optional<SubscribeOptions> options) {
  auto paf = js.newPromiseAndResolver<void>();
  struct ForEachState: public kj::Refcounted {
    jsg::Promise<void>::Resolver resolver;
    kj::Maybe<kj::Own<void>> handler;
    jsg::Ref<AbortController> visitorCallbackController = jsg::alloc<AbortController>();
    Visitor callback;
    uint32_t idx = 0;
    ForEachState(jsg::Promise<void>::Resolver resolver, Visitor callback)
        : resolver(kj::mv(resolver)), callback(kj::mv(callback)) {}
  };
  auto state = kj::refcounted<ForEachState>(kj::mv(paf.resolver), kj::mv(callback));

  SubscribeOptions internalOptions;
  kj::Vector<jsg::Ref<AbortSignal>> signals;
  signals.add(state->visitorCallbackController->getSignal());
  KJ_IF_SOME(opt, options) {
    KJ_IF_SOME(signal, opt.signal) {
      signals.add(signal.addRef());
    }
  }
  internalOptions.signal = AbortSignal::any(js, signals.releaseAsArray(), handler_);

  auto& s = KJ_ASSERT_NONNULL(internalOptions.signal);
  if (s->getAborted()) {
    state->resolver.reject(js, s->getReason(js));
    return kj::mv(paf.promise);
  }

  state->handler = s->newNativeHandler(js, kj::str("abort"),
      [&state=*state, signal=s.addRef()](jsg::Lock& js, auto event) mutable {
    state.resolver.reject(js, signal->getReason(js));
  }, true);

  subscribeImpl(js, kj::heap<Subscriber::InternalObserver>(
    [state=kj::addRef(*state)](jsg::Lock& js, jsg::JsValue value) mutable {
      js.tryCatch([&] {
        state->callback(js, value, state->idx);
        state->idx++;
      }, [&](jsg::Value exception) {
        auto error = jsg::JsValue(exception.getHandle(js));
        state->resolver.reject(js, error);
        state->visitorCallbackController->abort(js, error);
      });
    },
    [state=kj::addRef(*state)](jsg::Lock& js, jsg::JsValue error) mutable {
      state->resolver.reject(js, error);
    },
    [state=kj::addRef(*state)](jsg::Lock& js) mutable {
      state->resolver.resolve(js);
    }), kj::mv(internalOptions), handler_);

  return kj::mv(paf.promise);
}

jsg::Promise<bool> Observable::every(
    jsg::Lock& js,
    Predicate predicate,
    jsg::Optional<SubscribeOptions> options) {
  auto paf = js.newPromiseAndResolver<bool>();
  struct EveryState: public kj::Refcounted {
    jsg::Promise<bool>::Resolver resolver;
    jsg::Ref<AbortController> controller = jsg::alloc<AbortController>();
    Predicate predicate;
    kj::Maybe<kj::Own<void>> handler;
    uint32_t idx = 0;
    EveryState(jsg::Promise<bool>::Resolver resolver, Predicate predicate)
        : resolver(kj::mv(resolver)), predicate(kj::mv(predicate)) {}
  };
  auto state = kj::refcounted<EveryState>(kj::mv(paf.resolver), kj::mv(predicate));

  SubscribeOptions internalOptions;
  kj::Vector<jsg::Ref<AbortSignal>> signals;
  signals.add(state->controller->getSignal());
  KJ_IF_SOME(opts, options) {
    KJ_IF_SOME(signal, opts.signal) {
      signals.add(signal.addRef());
    }
  }
  internalOptions.signal = AbortSignal::any(js, signals.releaseAsArray(), handler_);
  auto& s KJ_ASSERT_NONNULL(internalOptions.signal);
  if (s->getAborted()) {
    state->resolver.reject(js, s->getReason(js));
    return kj::mv(paf.promise);
  }

  state->handler = s->newNativeHandler(js, kj::str("abort"),
      [&state=*state, signal=s.addRef()](jsg::Lock& js, auto event) mutable {
    state.resolver.reject(js, signal->getReason(js));
  }, true);

  subscribeImpl(js, kj::heap<Subscriber::InternalObserver>(
    [state=kj::addRef(*state)](jsg::Lock& js, jsg::JsValue value) mutable {
      js.tryCatch([&] {
        auto passed = state->predicate(js, value, state->idx);
        state->idx++;
        if (!passed) {
          state->resolver.resolve(js, false);
          state->controller->abort(js, kj::none);
        }
      }, [&](jsg::Value exception) {
        state->resolver.reject(js, jsg::JsValue(exception.getHandle(js)));
        state->controller->abort(js, jsg::JsValue(exception.getHandle(js)));
      });
    },
    [state=kj::addRef(*state)](jsg::Lock& js, jsg::JsValue error) mutable {
      state->resolver.reject(js, error);
    },
    [state=kj::addRef(*state)](jsg::Lock& js) mutable {
      state->resolver.resolve(js, true);
    }), kj::mv(internalOptions), handler_);

  return kj::mv(paf.promise);
}

jsg::Promise<jsg::JsRef<jsg::JsValue>> Observable::first(
    jsg::Lock& js,
    jsg::Optional<SubscribeOptions> options) {
  auto paf = js.newPromiseAndResolver<jsg::JsRef<jsg::JsValue>>();
  struct FirstState: public kj::Refcounted {
    kj::Maybe<kj::Own<void>> handler;
    kj::Maybe<jsg::Ref<AbortSignal>> signal;
    kj::Maybe<jsg::Promise<jsg::JsRef<jsg::JsValue>>::Resolver> resolver;
    jsg::Ref<AbortController> controller = jsg::alloc<AbortController>();
    FirstState(jsg::Promise<jsg::JsRef<jsg::JsValue>>::Resolver resolver)
        : resolver(kj::mv(resolver)) {}
  };
  auto state = kj::refcounted<FirstState>(kj::mv(paf.resolver));

  SubscribeOptions internalOptions;
  kj::Vector<jsg::Ref<AbortSignal>> signals;
  signals.add(state->controller->getSignal());
  KJ_IF_SOME(opts, options) {
    KJ_IF_SOME(signal, opts.signal) {
      signals.add(signal.addRef());
    }
  }
  internalOptions.signal = AbortSignal::any(js, signals.releaseAsArray(), handler_);
  auto& s = KJ_ASSERT_NONNULL(internalOptions.signal);
  if (s->getAborted()) {
    KJ_ASSERT_NONNULL(state->resolver).reject(js, s->getReason(js));
    return kj::mv(paf.promise);
  }
  state->signal = s.addRef();

  state->handler = s->newNativeHandler(js, kj::str("abort"),
      [&state=*state, &signal=*s](jsg::Lock& js, auto event) mutable {
    KJ_IF_SOME(resolver, state.resolver) {
      resolver.reject(js, signal.getReason(js));
    }
  }, true);

  subscribeImpl(js, kj::heap<Subscriber::InternalObserver>(
    [state=kj::addRef(*state)](jsg::Lock& js, jsg::JsValue value) mutable {
      auto& resolver = KJ_REQUIRE_NONNULL(state->resolver);
      resolver.resolve(js, jsg::JsRef(js, value));
      state->resolver = kj::none;
      state->controller->abort(js, kj::none);
    },
    [state=kj::addRef(*state)](jsg::Lock& js, jsg::JsValue error) mutable {
      KJ_IF_SOME(resolver, state->resolver) {
        resolver.reject(js, error);
      }
    },
    [state=kj::addRef(*state)](jsg::Lock& js) mutable {
      KJ_IF_SOME(resolver, state->resolver) {
        resolver.resolve(js, jsg::JsRef(js, js.undefined()));
      }
    }), kj::mv(internalOptions), handler_);

  return kj::mv(paf.promise);
}

jsg::Promise<jsg::JsRef<jsg::JsValue>> Observable::last(
    jsg::Lock& js,
    jsg::Optional<SubscribeOptions> options) {
  auto paf = js.newPromiseAndResolver<jsg::JsRef<jsg::JsValue>>();
  struct LastState: public kj::Refcounted {
    jsg::Promise<jsg::JsRef<jsg::JsValue>>::Resolver resolver;
    jsg::Ref<AbortController> controller = jsg::alloc<AbortController>();
    kj::Maybe<kj::Own<void>> handler;
    kj::Maybe<jsg::JsRef<jsg::JsValue>> lastValue;

    LastState(jsg::Promise<jsg::JsRef<jsg::JsValue>>::Resolver resolver)
        : resolver(kj::mv(resolver)) {}
  };
  auto state = kj::refcounted<LastState>(kj::mv(paf.resolver));

  KJ_IF_SOME(opts, options) {
    KJ_IF_SOME(signal, opts.signal) {
      if (signal->getAborted()) {
        state->resolver.reject(js, signal->getReason(js));
        return kj::mv(paf.promise);
      }
      state->handler = signal->newNativeHandler(js, kj::str("abort"),
          [&state=*state, signal=signal.addRef()](jsg::Lock& js, auto event) mutable {
        state.resolver.reject(js, signal->getReason(js));
      }, true);
    }
  }

  subscribeImpl(js, kj::heap<Subscriber::InternalObserver>(
    [state=kj::addRef(*state)](jsg::Lock& js, jsg::JsValue value) mutable {
      state->lastValue = jsg::JsRef(js, value);
    },
    [state=kj::addRef(*state)](jsg::Lock& js, jsg::JsValue error) mutable {
      state->resolver.reject(js, error);
    },
    [state=kj::addRef(*state)](jsg::Lock& js) mutable {
      KJ_IF_SOME(value, state->lastValue) {
        state->resolver.resolve(js, kj::mv(value));
      } else {
        state->resolver.resolve(js, jsg::JsRef(js, js.undefined()));
      }
    }), kj::mv(options), handler_);

  return kj::mv(paf.promise);
}

jsg::Promise<jsg::JsRef<jsg::JsValue>> Observable::find(
    jsg::Lock& js,
    Predicate predicate,
    jsg::Optional<SubscribeOptions> options) {
  auto paf = js.newPromiseAndResolver<jsg::JsRef<jsg::JsValue>>();
  struct FindState: kj::Refcounted {
    jsg::Promise<jsg::JsRef<jsg::JsValue>>::Resolver resolver;
    jsg::Ref<AbortController> controller = jsg::alloc<AbortController>();
    Predicate predicate;
    kj::Maybe<kj::Own<void>> handler;
    uint32_t idx = 0;
    FindState(jsg::Promise<jsg::JsRef<jsg::JsValue>>::Resolver resolver, Predicate predicate)
        : resolver(kj::mv(resolver)), predicate(kj::mv(predicate)) {}
  };
  auto state = kj::refcounted<FindState>(kj::mv(paf.resolver), kj::mv(predicate));

  SubscribeOptions internalOptions;
  kj::Vector<jsg::Ref<AbortSignal>> signals;
  signals.add(state->controller->getSignal());
  KJ_IF_SOME(opts, options) {
    KJ_IF_SOME(signal, opts.signal) {
      signals.add(signal.addRef());
    }
  }
  internalOptions.signal = AbortSignal::any(js, signals.releaseAsArray(), handler_);
  auto& s = KJ_ASSERT_NONNULL(internalOptions.signal);
  if (s->getAborted()) {
    state->resolver.reject(js, s->getReason(js));
    return kj::mv(paf.promise);
  }
  state->handler = s->newNativeHandler(js, kj::str("abort"),
      [&state=*state, signal=s.addRef()](jsg::Lock& js, auto event) mutable {
    state.resolver.reject(js, signal->getReason(js));
  }, true);

  subscribeImpl(js, kj::heap<Subscriber::InternalObserver>(
    [state=kj::addRef(*state)](jsg::Lock& js, jsg::JsValue value) mutable {
      js.tryCatch([&] {
        auto passed = state->predicate(js, value, state->idx);
        state->idx++;
        if (passed) {
          state->resolver.resolve(js, jsg::JsRef(js, value));
          state->controller->abort(js, kj::none);
        }
      }, [&](jsg::Value exception) {
        state->resolver.reject(js, jsg::JsValue(exception.getHandle(js)));
        state->controller->abort(js, jsg::JsValue(exception.getHandle(js)));
      });
    },
    [state=kj::addRef(*state)](jsg::Lock& js, jsg::JsValue error) mutable {
      state->resolver.reject(js, error);
    },
    [state=kj::addRef(*state)](jsg::Lock& js) mutable {
      state->resolver.resolve(js, jsg::JsRef(js, js.undefined()));
    }), kj::mv(internalOptions), handler_);

  return kj::mv(paf.promise);
}

jsg::Promise<bool> Observable::some(
    jsg::Lock& js,
    Predicate predicate,
    jsg::Optional<SubscribeOptions> options) {
  auto paf = js.newPromiseAndResolver<bool>();
  struct SomeState: public kj::Refcounted {
    jsg::Promise<bool>::Resolver resolver;
    jsg::Ref<AbortController> controller = jsg::alloc<AbortController>();
    Predicate predicate;
    kj::Maybe<kj::Own<void>> handler;
    uint32_t idx = 0;
    SomeState(jsg::Promise<bool>::Resolver resolver, Predicate predicate)
        : resolver(kj::mv(resolver)), predicate(kj::mv(predicate)) {}
  };
  auto state = kj::refcounted<SomeState>(kj::mv(paf.resolver), kj::mv(predicate));

  SubscribeOptions internalOptions;
  kj::Vector<jsg::Ref<AbortSignal>> signals;
  signals.add(state->controller->getSignal());
  KJ_IF_SOME(opts, options) {
    KJ_IF_SOME(signal, opts.signal) {
      signals.add(signal.addRef());
    }
  }
  internalOptions.signal = AbortSignal::any(js, signals.releaseAsArray(), handler_);
  auto& s = KJ_ASSERT_NONNULL(internalOptions.signal);
  if (s->getAborted()) {
    state->resolver.reject(js, s->getReason(js));
    return kj::mv(paf.promise);
  }
  state->handler = s->newNativeHandler(js, kj::str("abort"),
      [&state=*state, signal=s.addRef()](jsg::Lock& js, auto event) mutable {
    state.resolver.reject(js, signal->getReason(js));
  }, true);

  subscribeImpl(js, kj::heap<Subscriber::InternalObserver>(
    [state=kj::addRef(*state)](jsg::Lock& js, jsg::JsValue value) mutable {
      js.tryCatch([&] {
        auto passed = state->predicate(js, value, state->idx);
        state->idx++;
        if (passed) {
          state->resolver.resolve(js, true);
          state->controller->abort(js, kj::none);
        }
      }, [&](jsg::Value exception) {
        state->resolver.reject(js, jsg::JsValue(exception.getHandle(js)));
        state->controller->abort(js, jsg::JsValue(exception.getHandle(js)));
      });
    },
    [state=kj::addRef(*state)](jsg::Lock& js, jsg::JsValue error) mutable {
      state->resolver.reject(js, error);
    },
    [state=kj::addRef(*state)](jsg::Lock& js) mutable {
      state->resolver.resolve(js, false);
    }), kj::mv(internalOptions), handler_);

  return kj::mv(paf.promise);
}

jsg::Promise<jsg::JsRef<jsg::JsValue>> Observable::reduce(
    jsg::Lock& js,
    Reducer reducer,
    jsg::Optional<jsg::JsValue> initialValue,
    jsg::Optional<SubscribeOptions> options) {
  auto paf = js.newPromiseAndResolver<jsg::JsRef<jsg::JsValue>>();
  struct ReduceState: public kj::Refcounted {
    jsg::Promise<jsg::JsRef<jsg::JsValue>>::Resolver resolver;
    jsg::Ref<AbortController> controller = jsg::alloc<AbortController>();
    Reducer reducer;
    jsg::JsRef<jsg::JsValue> accumulator;
    kj::Maybe<kj::Own<void>> handler;
    ReduceState(jsg::Promise<jsg::JsRef<jsg::JsValue>>::Resolver resolver, Reducer reducer)
        : resolver(kj::mv(resolver)), reducer(kj::mv(reducer)) {}
  };
  auto state = kj::refcounted<ReduceState>(kj::mv(paf.resolver), kj::mv(reducer));

  SubscribeOptions internalOptions;
  kj::Vector<jsg::Ref<AbortSignal>> signals;
  signals.add(state->controller->getSignal());
  KJ_IF_SOME(opts, options) {
    KJ_IF_SOME(signal, opts.signal) {
      signals.add(signal.addRef());
    }
  }
  internalOptions.signal = AbortSignal::any(js, signals.releaseAsArray(), handler_);
  auto& s = KJ_ASSERT_NONNULL(internalOptions.signal);

  if (s->getAborted()) {
    state->resolver.reject(js, s->getReason(js));
    return kj::mv(paf.promise);
  }
  state->handler = s->newNativeHandler(js, kj::str("abort"),
      [&state=*state, signal=s.addRef()](jsg::Lock& js, auto event) mutable {
    state.resolver.reject(js, signal->getReason(js));
  }, true);

  KJ_IF_SOME(iv, initialValue) {
    state->accumulator = jsg::JsRef(js, iv);
  }

  subscribeImpl(js, kj::heap<Subscriber::InternalObserver>(
    [state=kj::addRef(*state)](jsg::Lock& js, jsg::JsValue value) mutable {
      js.tryCatch([&] {
        state->accumulator = jsg::JsRef(js,
            state->reducer(js, state->accumulator.getHandle(js), value));
      }, [&](jsg::Value exception) {
        state->resolver.reject(js, jsg::JsValue(exception.getHandle(js)));
        state->controller->abort(js, jsg::JsValue(exception.getHandle(js)));
      });
    },
    [state=kj::addRef(*state)](jsg::Lock& js, jsg::JsValue error) mutable {
      state->resolver.reject(js, error);
    },
    [state=kj::addRef(*state)](jsg::Lock& js) mutable {
      state->resolver.resolve(js, kj::mv(state->accumulator));
    }), kj::mv(internalOptions), handler_);

  return kj::mv(paf.promise);
}

void Observable::visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
  tracker.trackField("callback_", callback_);
}

namespace {
jsg::Promise<void> asyncGenLoop(
    jsg::Lock& js,
    jsg::AsyncGenerator<jsg::JsRef<jsg::JsValue>> gen,
    jsg::Ref<Subscriber> subscriber) {
  return gen.next(js).then(js,
      [subscriber=subscriber.addRef(),
       gen=kj::mv(gen)](jsg::Lock& js, kj::Maybe<jsg::JsRef<jsg::JsValue>> value) mutable {
    KJ_IF_SOME(v, value) {
      subscriber->next(js, v.getHandle(js));
      return asyncGenLoop(js, kj::mv(gen), kj::mv(subscriber));
    }
    subscriber->complete(js);
    return js.resolvedPromise();
  }, [subscriber=subscriber.addRef()](jsg::Lock& js, jsg::Value error) mutable {
    subscriber->error(js, jsg::JsValue(error.getHandle(js)));
    return js.resolvedPromise();
  });
}
}  // namespace

void Observable::visitForGc(jsg::GcVisitor& visitor) {
  visitor.visit(callback_);
}

jsg::Ref<Observable> Observable::from(
    jsg::Lock& js,
    jsg::JsValue value,
    const jsg::TypeHandler<HandlerFunction>& handler,
    const jsg::TypeHandler<jsg::Ref<Observable>>& observableHandler,
    const jsg::TypeHandler<jsg::Promise<jsg::JsRef<jsg::JsValue>>>& promiseHandler,
    const jsg::TypeHandler<jsg::AsyncGenerator<jsg::JsRef<jsg::JsValue>>>& asyncGeneratorHandler) {

  KJ_IF_SOME(observable, observableHandler.tryUnwrap(js, value)) {
    // If the value is already an Observable, just return it.
    return kj::mv(observable);
  }

  if (value.isPromise()) {
    struct PromiseState {
      jsg::Promise<jsg::JsRef<jsg::JsValue>> promise;
    };
    auto promise = KJ_ASSERT_NONNULL(promiseHandler.tryUnwrap(js, value));
    return jsg::alloc<Observable>(js,
        JSG_VISITABLE_LAMBDA(
            (state=PromiseState { .promise = kj::mv(promise) }),
            (state.promise),
            (jsg::Lock& js, jsg::Ref<Subscriber> subscriber) {
          state.promise = state.promise.then(js, JSG_VISITABLE_LAMBDA(
              (subscriber=subscriber.addRef()),
              (subscriber),
              (jsg::Lock& js, jsg::JsRef<jsg::JsValue> value) {
                subscriber->next(js, value.getHandle(js));
                subscriber->complete(js);
                return kj::mv(value);
              }), JSG_VISITABLE_LAMBDA(
              (subscriber=subscriber.addRef()),
              (subscriber),
              (jsg::Lock& js, jsg::Value exception) {
                subscriber->error(js, jsg::JsValue(exception.getHandle(js)));
                return jsg::JsRef(js, js.undefined());
              }));
        }), handler, observableHandler, promiseHandler, asyncGeneratorHandler);
  }

  KJ_IF_SOME(gen, asyncGeneratorHandler.tryUnwrap(js, value)) {
    return jsg::alloc<Observable>(js,
        JSG_VISITABLE_LAMBDA(
            (gen=kj::mv(gen)),
            (gen),
            (jsg::Lock& js, jsg::Ref<Subscriber> subscriber) mutable {
          if (subscriber->getSignal()->getAborted()) return;
          asyncGenLoop(js, kj::mv(gen), kj::mv(subscriber));
        }), handler, observableHandler, promiseHandler, asyncGeneratorHandler);
  }

  JSG_FAIL_REQUIRE(TypeError, "Value is not an Observable, Promise, Generator, or AsyncGenerator");
}

jsg::Ref<Observable> addObservableHandler(
    jsg::Lock& js,
    jsg::Ref<EventTarget> eventTarget,
    kj::String type,
    jsg::Optional<ObservableEventListenerOptions> options,
    const jsg::TypeHandler<HandlerFunction>& handler,
    const jsg::TypeHandler<jsg::Ref<Observable>>& observableHandler,
    const jsg::TypeHandler<jsg::Promise<jsg::JsRef<jsg::JsValue>>>& promiseHandler,
    const jsg::TypeHandler<jsg::AsyncGenerator<jsg::JsRef<jsg::JsValue>>>& asyncGeneratorHandler,
    const jsg::TypeHandler<jsg::Ref<Event>>& eventHandler) {
  return jsg::alloc<Observable>(js,
      JSG_VISITABLE_LAMBDA(
          (eventTarget=kj::mv(eventTarget),
           type=kj::mv(type),
           options=kj::mv(options),
           &eventHandler),
          (eventTarget),
          (jsg::Lock& js, jsg::Ref<Subscriber> subscriber) {
        if (subscriber->getSignal()->getAborted()) return;
        ObservableEventListenerOptions opts = options.orDefault({});

        // We ignore the capture and passive options for now.
        subscriber->getObservable().setNativeHandler(
            eventTarget->newNativeHandler(js, kj::mv(type),
                [&subscriber=*subscriber, &eventHandler]
                (jsg::Lock& js, jsg::Ref<Event> event) mutable {
          auto obj = eventHandler.wrap(js, kj::mv(event));
          subscriber.next(js, jsg::JsValue(obj));
        }, false).attach(eventTarget.addRef(), kj::mv(subscriber)));
      }),
      handler, observableHandler, promiseHandler, asyncGeneratorHandler);
}

}  // namespace workerd::api
