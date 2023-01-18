// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#include "async-context.h"
#include "jsg.h"
#include "setup.h"
#include <v8.h>

namespace workerd::jsg {

AsyncContextFrame::AsyncContextFrame(Lock& js, StorageEntry storageEntry)
    : isolate(IsolateBase::from(js.v8Isolate)) {
  // Lazily enables the hooks for async context tracking.
  isolate.setAsyncContextTrackingEnabled();

  KJ_IF_MAYBE(frame, current(js)) {
    // Propagate the storage context of the current frame (if any).
    // If current(js) returns nullptr, we assume we're in the root
    // frame and there is no storage to propagate.
    frame->storage.eraseAll([](const auto& entry) { return entry.key->isDead(); });
    for (auto& entry : frame->storage) {
      storage.insert(entry.clone(js));
    }
  }

  storage.upsert(kj::mv(storageEntry), [](StorageEntry& existing, StorageEntry&& row) mutable {
    existing.value = kj::mv(row.value);
  });
}

kj::Maybe<AsyncContextFrame&> AsyncContextFrame::tryGetContext(
    Lock& js,
    v8::Local<v8::Promise> promise) {
  auto handle = js.getPrivateSymbolFor(Lock::PrivateSymbols::ASYNC_CONTEXT);
  // We do not use the normal unwrapOpaque here since that would consume the wrapped
  // value, and we need to be able to unwrap multiple times.
  auto ref = check(promise->GetPrivate(js.v8Isolate->GetCurrentContext(), handle));
  KJ_IF_MAYBE(wrappable, Wrappable::tryUnwrapOpaque(js.v8Isolate, ref)) {
    AsyncContextFrame* frame = dynamic_cast<AsyncContextFrame*>(wrappable);
    KJ_ASSERT(frame != nullptr);
    return *frame;
  }
  return nullptr;
}

kj::Maybe<AsyncContextFrame&> AsyncContextFrame::tryGetContext(
    Lock& js,
    V8Ref<v8::Promise>& promise) {
  return tryGetContext(js, promise.getHandle(js));
}

kj::Maybe<AsyncContextFrame&> AsyncContextFrame::current(Lock& js) {
  auto& isolateBase = IsolateBase::from(js.v8Isolate);
  if (isolateBase.asyncFrameStack.size() == 0) {
    return nullptr;
  }
  KJ_SWITCH_ONEOF(isolateBase.asyncFrameStack.back()) {
    KJ_CASE_ONEOF(frame, AsyncContextFrame*) {
      return *frame;
    }
    KJ_CASE_ONEOF(root, IsolateBase::RootAsyncContextFrame) {
      // In this case, the logical root frame has been pushed onto the
      // top of the stack. This effectively means that no storage context
      // is active, so we just return nullptr.
      return nullptr;
    }
  }
  KJ_UNREACHABLE;
}

Ref<AsyncContextFrame> AsyncContextFrame::create(Lock& js, StorageEntry storageEntry) {
  return alloc<AsyncContextFrame>(js, kj::mv(storageEntry));
}

v8::Local<v8::Function> AsyncContextFrame::wrap(
    Lock& js, V8Ref<v8::Function>& fn,
    kj::Maybe<v8::Local<v8::Value>> thisArg) {
  return wrap(js, fn.getHandle(js), thisArg);
}

v8::Local<v8::Function> AsyncContextFrame::wrap(
    Lock& js,
    v8::Local<v8::Function> fn,
    kj::Maybe<v8::Local<v8::Value>> thisArg) {
  auto isolate = js.v8Isolate;
  auto context = isolate->GetCurrentContext();

  return js.wrapReturningFunction(context, JSG_VISITABLE_LAMBDA(
      (
        frame = JSG_THIS,
        thisArg = js.v8Ref(thisArg.orDefault(context->Global())),
        fn = js.v8Ref(fn)
      ),
      (frame, thisArg, fn),
      (Lock& js, const v8::FunctionCallbackInfo<v8::Value>& args) {
    auto function = fn.getHandle(js);
    auto context = js.v8Isolate->GetCurrentContext();

    kj::Vector<v8::Local<v8::Value>> argv(args.Length());
    for (int n = 0; n < args.Length(); n++) {
      argv.add(args[n]);
    }

    AsyncContextFrame::Scope scope(js, *frame.get());
    v8::Local<v8::Value> result;
    return check(function->Call(context, thisArg.getHandle(js), args.Length(), argv.begin()));
  }));
}

v8::Local<v8::Function> AsyncContextFrame::wrapRoot(
    Lock& js,
    v8::Local<v8::Function> fn,
    kj::Maybe<v8::Local<v8::Value>> thisArg) {
  auto isolate = js.v8Isolate;
  auto context = isolate->GetCurrentContext();

  return js.wrapReturningFunction(context, JSG_VISITABLE_LAMBDA(
      (
        thisArg = js.v8Ref(thisArg.orDefault(context->Global())),
        fn = js.v8Ref(fn)
      ),
      (thisArg, fn),
      (Lock& js, const v8::FunctionCallbackInfo<v8::Value>& args) {
    auto function = fn.getHandle(js);
    auto context = js.v8Isolate->GetCurrentContext();

    kj::Vector<v8::Local<v8::Value>> argv(args.Length());
    for (int n = 0; n < args.Length(); n++) {
      argv.add(args[n]);
    }

    AsyncContextFrame::Scope scope(js, nullptr);
    v8::Local<v8::Value> result;
    return check(function->Call(context, thisArg.getHandle(js), args.Length(), argv.begin()));
  }));
}

void AsyncContextFrame::attachContext(Lock& js, v8::Local<v8::Promise> promise) {
  auto handle = js.getPrivateSymbolFor(Lock::PrivateSymbols::ASYNC_CONTEXT);
  auto context = js.v8Isolate->GetCurrentContext();
  KJ_DASSERT(!check(promise->HasPrivate(context, handle)));
  KJ_ASSERT(check(promise->SetPrivate(context, handle, getJSWrapper(js))));
}

kj::Maybe<Value&> AsyncContextFrame::get(StorageKey& key) {
  KJ_ASSERT(!key.isDead());
  storage.eraseAll([](const auto& entry) { return entry.key->isDead(); });
  return storage.find(key).map([](auto& entry) -> Value& { return entry.value; });
}

AsyncContextFrame::Scope::Scope(Lock& js, kj::Maybe<AsyncContextFrame&> resource)
    : Scope(js.v8Isolate, resource) {}

AsyncContextFrame::Scope::Scope(v8::Isolate* isolate, kj::Maybe<AsyncContextFrame&> frame)
    : isolate(IsolateBase::from(isolate)) {
  KJ_IF_MAYBE(f, frame) {
    this->isolate.pushAsyncFrame(*f);
  } else {
    this->isolate.pushRootAsyncFrame();
  }
}

AsyncContextFrame::Scope::~Scope() noexcept(false) {
  isolate.popAsyncFrame();
}

AsyncContextFrame::StorageScope::StorageScope(
    Lock& js,
    StorageKey& key,
    Value store)
    : frame(AsyncContextFrame::create(js, StorageEntry {
        .key = kj::addRef(key),
        .value = kj::mv(store)
      })),
      scope(js, *frame) {}

v8::Local<v8::Object> AsyncContextFrame::getJSWrapper(Lock& js) {
  KJ_IF_MAYBE(handle, tryGetHandle(js.v8Isolate)) {
    return *handle;
  }
  return attachOpaqueWrapper(js.v8Isolate->GetCurrentContext(), true);
}

void AsyncContextFrame::jsgVisitForGc(GcVisitor& visitor) {
  for (auto& entry : storage) {
    visitor.visit(entry.value);
  }
}

void IsolateBase::pushAsyncFrame(AsyncContextFrame& next) {
  asyncFrameStack.add(&next);
}

void IsolateBase::pushRootAsyncFrame() {
  asyncFrameStack.add(RootAsyncContextFrame{});
}

void IsolateBase::popAsyncFrame() {
  KJ_DASSERT(asyncFrameStack.size() > 0, "the async context frame stack was corrupted");
  asyncFrameStack.removeLast();
}

void IsolateBase::setAsyncContextTrackingEnabled() {
  // Enabling async context tracking installs a relatively expensive callback on the v8 isolate
  // that attaches additional metadata to every promise created. The additional metadata is used
  // to implement support for the Node.js AsyncLocalStorage API. Since that is the only current
  // use for it, we only install the promise hook when that api is used.
  if (asyncContextTrackingEnabled) return;
  asyncContextTrackingEnabled = true;
  ptr->SetPromiseHook(&promiseHook);
}

void IsolateBase::promiseHook(v8::PromiseHookType type,
                              v8::Local<v8::Promise> promise,
                              v8::Local<v8::Value> parent) {
  auto isolate = promise->GetIsolate();

  // V8 will call the promise hook even while execution is terminating. In that
  // case we don't want to do anything here.
  if (isolate->IsExecutionTerminating() || isolate->IsDead()) {
    return;
  }

  // This is a fairly expensive method. It is invoked at least once, and a most
  // four times for every JavaScript promise that is created within an isolate.
  // Accordingly, the hook is only installed when the AsyncLocalStorage API is
  // used.

  auto& js = Lock::from(isolate);
  auto& isolateBase = IsolateBase::from(isolate);

  const auto isRejected = [&] { return promise->State() == v8::Promise::PromiseState::kRejected; };

  // TODO(later): The try/catch block here echoes the semantics of LiftKj.
  // We don't use LiftKj here because that currently requires a FunctionCallbackInfo,
  // which we don't have (or want here). If we end up needing this pattern elsewhere,
  // we can implement a variant of LiftKj that does so and switch this over to use it.
  try {
    switch (type) {
      case v8::PromiseHookType::kInit: {
        // The kInit event is triggered by v8 when a deferred Promise is created. This
        // includes all calls to `new Promise(...)`, `then()`, `catch()`, `finally()`,
        // uses of `await ...`, `Promise.all()`, etc.
        // Whenever a Promise is created, we associate it with the current AsyncContextFrame.
        KJ_IF_MAYBE(frame, AsyncContextFrame::current(js)) {
          frame->attachContext(js, promise);
        }
        break;
      }
      case v8::PromiseHookType::kBefore: {
        // The kBefore event is triggered immediately before a Promise continuation.
        // We use it here to enter the AsyncContextFrame that was associated with the
        // promise when it was created.
        KJ_IF_MAYBE(frame, AsyncContextFrame::tryGetContext(js, promise)) {
          isolateBase.pushAsyncFrame(*frame);
        } else {
          isolateBase.pushRootAsyncFrame();
        }
        // We do not use AsyncContextFrame::Scope here because we do not exit the frame
        // until the kAfter event fires.
        break;
      }
      case v8::PromiseHookType::kAfter: {
        isolateBase.popAsyncFrame();

        // If the promise has been rejected here, we have to maintain the association of the
        // async context to the promise so that the context can be propagated to the unhandled
        // rejection handler. However, if the promise has been fulfilled, we do not expect
        // the context to be used any longer so we can break the context association here and
        // allow the opaque wrapper to be garbage collected.
        if (!isRejected()) {
          auto handle = js.getPrivateSymbolFor(Lock::PrivateSymbols::ASYNC_CONTEXT);
          check(promise->DeletePrivate(js.v8Isolate->GetCurrentContext(), handle));
        }

        break;
      }
      case v8::PromiseHookType::kResolve: {
        // This case is a bit different. As an optimization, it appears that v8 will skip
        // the kInit, kBefore, and kAfter events for Promises that are immediately resolved (e.g.
        // Promise.resolve, and Promise.reject) and instead will emit the kResolve event first.
        // When this event occurs, and the promise is rejected, we need to check to see if the
        // promise is already wrapped, and if it is not, do so.
        KJ_IF_MAYBE(current, AsyncContextFrame::current(js)) {
          if (isRejected() && AsyncContextFrame::tryGetContext(js, promise) == nullptr) {
            current->attachContext(js, promise);
          }
        }
        break;
      }
    }
  } catch (JsExceptionThrown&) {
    // Catching JsExceptionThrown implies that an exception is already scheduled on the isolate
    // so we don't need to throw it again, just allow it to bubble up and out.
  } catch (std::exception& ex) {
    // This case is purely defensive and is included really just to align with the
    // semantics in LiftKj. We'd be using LiftKj here already if that didn't require
    // use of a FunctionCallbackInfo.
    throwInternalError(isolate, ex.what());
  } catch (kj::Exception& ex) {
    throwInternalError(isolate, kj::mv(ex));
  } catch (...) {
    throwInternalError(isolate, kj::str("caught unknown exception of type: ",
                                        kj::getCaughtExceptionType()));
  }
}

}  // namespace workerd::jsg
