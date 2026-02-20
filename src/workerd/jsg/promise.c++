// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "async-context.h"
#include "jsg.h"

namespace workerd::jsg {

namespace {
template <typename T>
v8::Local<T> getLocal(v8::Isolate* isolate, v8::Global<T>& global) {
  if (!global.IsEmpty()) {
    return global.Get(isolate);
  }
  return v8::Local<T>();
};

kj::Maybe<Ref<AsyncContextFrame>> getFrameRef(jsg::Lock& js) {
  return AsyncContextFrame::current(js).map(
      [](AsyncContextFrame& frame) -> Ref<AsyncContextFrame> { return frame.addRef(); });
}

kj::Maybe<AsyncContextFrame&> tryGetFrame(kj::Maybe<Ref<AsyncContextFrame>>& maybeFrame) {
  return maybeFrame.map(
      [](Ref<AsyncContextFrame>& frame) -> AsyncContextFrame& { return *frame.get(); });
}
}  // namespace

UnhandledRejectionHandler::UnhandledRejection::UnhandledRejection(jsg::Lock& js,
    jsg::V8Ref<v8::Promise> promise,
    jsg::Value value,
    v8::Local<v8::Message> message)
    : hash(kj::hashCode(promise.getHandle(js)->GetIdentityHash())),
      promise(js.v8Isolate, promise.getHandle(js)),
      value(js.v8Isolate, value.getHandle(js)),
      message(js.v8Isolate, message),
      asyncContextFrame(getFrameRef(js)) {}

void UnhandledRejectionHandler::report(
    Lock& js, v8::PromiseRejectEvent event, jsg::V8Ref<v8::Promise> promise, jsg::Value value) {
  js.tryCatch([&] {
    switch (event) {
      case v8::PromiseRejectEvent::kPromiseRejectWithNoHandler: {
        rejectedWithNoHandler(js, kj::mv(promise), kj::mv(value));
        return;
      }
      case v8::PromiseRejectEvent::kPromiseHandlerAddedAfterReject: {
        handledAfterRejection(js, kj::mv(promise));
        return;
      }
      case v8::PromiseRejectEvent::kPromiseRejectAfterResolved: {
        break;
      }
      case v8::PromiseRejectEvent::kPromiseResolveAfterResolved: {
        break;
      }
    }
  }, [&](Value exception) {
    // Exceptions here should be rare but possible. Any errors that occur
    // here are likely fatal to the worker. This handling helps us avoid
    // crashing. We'll log the error hand continue.
    if (js.areWarningsLogged()) {
      js.logWarning(kj::str("There was an error while reporting an unhandled promise rejection: ",
          exception.getHandle(js)));
    }
  });
}

UnhandledRejectionHandler::UnhandledRejection::~UnhandledRejection() {
  if (promise.IsWeak()) {
    promise.ClearWeak();
  }
  if (value.IsWeak()) {
    value.ClearWeak();
  }
}

void UnhandledRejectionHandler::clear() {
  warnedRejections.clear();
  unhandledRejections.clear();
}

void UnhandledRejectionHandler::rejectedWithNoHandler(
    jsg::Lock& js, jsg::V8Ref<v8::Promise> promise, jsg::V8Ref<v8::Value> value) {
  auto message = v8::Exception::CreateMessage(js.v8Isolate, value.getHandle(js));

  // It's not yet clear under what conditions it happens, but this can be called
  // twice with the same promise. It really shouldn't happen in the regular cases
  // but we address the edge case by using upsert and just replacing the existing
  // value and message when it does.

  unhandledRejections.upsert(
      UnhandledRejection(js, kj::mv(promise), kj::mv(value), kj::mv(message)),
      [&](UnhandledRejection& existing, UnhandledRejection&& replacement) {
    // Replacing the promise here is defensive, since they have the same hash
    // it *should* be the same promise, but let's be sure. We don't need to
    // assert here because the book keeping on this is not critical.
    existing = kj::mv(replacement);
  });

  ensureProcessingWarnings(js);
}

void UnhandledRejectionHandler::handledAfterRejection(
    jsg::Lock& js, jsg::V8Ref<v8::Promise> promise) {
  // If an unhandled rejection is found in the table, then all we need to do is erase it.
  // If it's not found, then we'll skip on to the next step of determining if we've already
  // emitted an unhandled rejection warning about this promise to determine if we need to
  // emit another warning indicating that it's been handled.
  KJ_DEFER(ensureProcessingWarnings(js));

  HashedPromise key(promise.getHandle(js));

  if (unhandledRejections.eraseMatch(key)) {
    return;
  }

  KJ_IF_SOME(item, warnedRejections.find(key)) {
    auto promise = getLocal(js.v8Isolate, item.promise);
    if (!promise.IsEmpty()) {
      AsyncContextFrame::Scope scope(js, tryGetFrame(item.asyncContextFrame));
      handler(js, v8::kPromiseHandlerAddedAfterReject, jsg::HashableV8Ref(js.v8Isolate, promise),
          js.v8Ref(js.v8Undefined()));
    }
    warnedRejections.release(item);
  }
}

void UnhandledRejectionHandler::ensureProcessingWarnings(jsg::Lock& js) {
  if (scheduled) {
    return;
  }
  scheduled = true;
  if (useMicrotasksCompletedCallback) {
    // Schedule processing to run after the microtask checkpoint completes.
    // This ensures that promise chains like `.then().catch()` have fully settled
    // before we decide a rejection is unhandled. Using a microtask would race
    // with V8's internal promise adoption microtasks and fire too early.
    // See https://github.com/cloudflare/workerd/issues/6020
    js.v8Isolate->AddMicrotasksCompletedCallback(
        &UnhandledRejectionHandler::onMicrotasksCompleted, this);
    // Ensure we get another microtask checkpoint to deliver the callback even if
    // we're already past the current one.
    js.requestExtraMicrotaskCheckpoint();
  } else {
    js.resolvedPromise().then(js, [this](jsg::Lock& js) { processWarnings(js); });
  }
}

void UnhandledRejectionHandler::onMicrotasksCompleted(v8::Isolate* isolate, void* data) {
  auto* handler = static_cast<UnhandledRejectionHandler*>(data);
  KJ_DEFER(isolate->RemoveMicrotasksCompletedCallback(
      &UnhandledRejectionHandler::onMicrotasksCompleted, data));
  auto& js = Lock::from(isolate);
  KJ_TRY {
    handler->processWarnings(js);

    // Ensure microtasks scheduled by unhandledrejection handlers run promptly.
    js.requestExtraMicrotaskCheckpoint();
  }
  KJ_CATCH(exception) {
    handler->scheduled = false;
    KJ_LOG(ERROR, "uncaught exception while processing unhandled rejections", exception);
  }
}

void UnhandledRejectionHandler::processWarnings(jsg::Lock& js) {
  scheduled = false;
  warnedRejections.eraseAll([](auto& value) { return !value.isAlive(); });

  while (unhandledRejections.size() > 0) {
    auto entry = unhandledRejections.release(*unhandledRejections.begin());

    if (!entry.isAlive()) {
      continue;
    }

    auto promise = getLocal(js.v8Isolate, entry.promise);
    auto value = getLocal(js.v8Isolate, entry.value);

    AsyncContextFrame::Scope scope(js, tryGetFrame(entry.asyncContextFrame));

    // Most of the time it shouldn't be found but there are times where it can
    // be duplicated -- such as when a promise gets rejected multiple times.
    // Check quickly before inserting to avoid a crash.
    // Keep strong refs through dispatch, then downgrade to weak to avoid leaks.
    entry.promise.SetWeak();
    entry.value.SetWeak();
    warnedRejections.upsert(
        kj::mv(entry), [](UnhandledRejection& existing, UnhandledRejection&& replacement) {
      // We're just going to ignore if the unhandled rejection was already here.
    });

    js.tryCatch([&] {
      handler(js, v8::kPromiseRejectWithNoHandler, jsg::HashableV8Ref(js.v8Isolate, promise),
          js.v8Ref(value));
    }, [&](Value exception) {
      // If any exceptions occur while reporting the event, we will log them
      // but otherwise ignore them. We do not want such errors to be fatal here.
      if (js.areWarningsLogged()) {
        js.logWarning(
            kj::str("Exception while logging unhandled rejection:", exception.getHandle(js)));
      }
    });
  }
}

void UnhandledRejectionHandler::UnhandledRejection::visitForMemoryInfo(
    MemoryTracker& tracker) const {
  tracker.trackField("asyncContextFrame", asyncContextFrame);
}

}  // namespace workerd::jsg
