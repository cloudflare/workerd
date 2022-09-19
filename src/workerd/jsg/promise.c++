// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

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
}  // namespace

UnhandledRejectionHandler::UnhandledRejection::UnhandledRejection(
    jsg::Lock& js,
    jsg::V8Ref<v8::Promise> promise,
    jsg::Value value,
    v8::Local<v8::Message> message,
    size_t rejectionNumber)
    : hash(promise.getHandle(js)->GetIdentityHash()),
      promise(js.v8Isolate, promise.getHandle(js)),
      value(js.v8Isolate, value.getHandle(js)),
      message(js.v8Isolate, message),
      rejectionNumber(rejectionNumber) {
  this->promise.SetWeak();
  this->value.SetWeak();
}

void UnhandledRejectionHandler::report(
    Lock& js,
    v8::PromiseRejectEvent event,
    jsg::V8Ref<v8::Promise> promise,
    jsg::Value value) {
  js.tryCatch([&] {
    switch (event) {
      case v8::PromiseRejectEvent::kPromiseRejectWithNoHandler: {
        return rejectedWithNoHandler(js, kj::mv(promise), kj::mv(value));
      }
      case v8::PromiseRejectEvent::kPromiseHandlerAddedAfterReject: {
        return handledAfterRejection(js, kj::mv(promise));
      }
      case v8::PromiseRejectEvent::kPromiseRejectAfterResolved: {
        break;
      }
      case v8::PromiseRejectEvent::kPromiseResolveAfterResolved: {
        break;
      }
    }
  }, [&] (Value exception) {
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
    jsg::Lock& js,
    jsg::V8Ref<v8::Promise> promise,
    jsg::V8Ref<v8::Value> value) {
  auto message = v8::Exception::CreateMessage(js.v8Isolate, value.getHandle(js.v8Isolate));

  // It's not yet clear under what conditions it happens, but this can be called
  // twice with the same promise. It really shouldn't happen in the regular cases
  // but we address the edge case by using upsert and just replacing the existing
  // value and message when it does.

  unhandledRejections.upsert(UnhandledRejection(
      js,
      kj::mv(promise),
      kj::mv(value),
      kj::mv(message),
      ++rejectionCount), [&](UnhandledRejection& existing, UnhandledRejection&& replacement) {
    // Replacing the promise here is defensive, since they have the same hash
    // it *should* be the same promise, but let's be sure. We don't need to
    // assert here because the book keeping on this is not critical.
    existing = kj::mv(replacement);
  });

  ensureProcessingWarnings(js);
}

void UnhandledRejectionHandler::handledAfterRejection(
    jsg::Lock& js,
    jsg::V8Ref<v8::Promise> promise) {
  // If an unhandled rejection is found in the table, then all we need to do is erase it.
  // If it's not found, then we'll skip on to the next step of determining if we've already
  // emitted an unhandled rejection warning about this promise to determine if we need to
  // emit another warning indicating that it's been handled.
  KJ_DEFER(ensureProcessingWarnings(js));

  uint hash = promise.getHandle(js)->GetIdentityHash();

  if (unhandledRejections.eraseMatch(hash)) {
    return;
  }

  KJ_IF_MAYBE(item, warnedRejections.find(hash)) {
    auto promise = getLocal(js.v8Isolate, item->promise);
    if (!promise.IsEmpty()) {
      // TODO(later): Chromium handles this differently... essentially when the
      // inspector log is created, chromium will revoke the log entry here instead
      // of printing a new warning (causing the previously printed warning to disappear
      // from the inspector console). We currently don't appear to have a way of
      // doing that yet, so printing the warning is the next best thing.
      if (js.areWarningsLogged()) {
        js.logWarning(
            kj::str("A promise rejection was handled asynchronously. This warning "
                    "occurs when attaching a catch handler to a promise after it "
                    "rejected. (rejection #", item->rejectionNumber, ")"));
      }

      handler(js, v8::kPromiseHandlerAddedAfterReject,
              jsg::HashableV8Ref(js.v8Isolate, promise),
              js.v8Ref(js.v8Undefined()));
    }
    warnedRejections.release(*item);
  }
}

void UnhandledRejectionHandler::ensureProcessingWarnings(jsg::Lock& js) {
  if (scheduled) {
    return;
  }
  scheduled = true;
  js.resolvedPromise().then(js, [this](jsg::Lock& js) {
    scheduled = false;
    warnedRejections.eraseAll([](auto& value) { return !value.isAlive(); });

    while (unhandledRejections.size() > 0) {
      auto entry = unhandledRejections.release(*unhandledRejections.begin());

      if (!entry.isAlive()) {
        continue;
      }

      auto promise = getLocal(js.v8Isolate, entry.promise);
      auto value = getLocal(js.v8Isolate, entry.value);

      // Most of the time it shouldn't be found but there are times where it can
      // be duplicated -- such as when a promise gets rejected multiple times.
      // Check quickly before inserting to avoid a crash.
      warnedRejections.upsert(
          kj::mv(entry),
          [](UnhandledRejection& existing, UnhandledRejection&& replacement) {
        // We're just going to ignore if the unhandled rejection was already here.
      });

      js.tryCatch([&] {
        handler(js, v8::kPromiseRejectWithNoHandler,
                jsg::HashableV8Ref(js.v8Isolate, promise),
                js.v8Ref(value));
      }, [&](Value exception) {
        // If any exceptions occur while reporting the event, we will log them
        // but otherwise ignore them. We do not want such errors to be fatal here.
        if (js.areWarningsLogged()) {
          js.logWarning(kj::str("Exception while logging unhandled rejection:",
                                exception.getHandle(js)));
        }
      });
    }
  });
}

}  // namespace workerd::jsg
