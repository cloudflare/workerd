// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#include "async-context.h"
#include "jsg.h"
#include <workerd/jsg/memory.h>
#include <v8.h>

namespace workerd::jsg {

namespace {
inline void maybeSetV8ContinuationContext(
    v8::Isolate* isolate,
    kj::Maybe<AsyncContextFrame&> maybeFrame) {
  v8::Local<v8::Value> value;
  KJ_IF_SOME(frame, maybeFrame) {
    value = frame.getJSWrapper(isolate);
  } else {
    value = v8::Undefined(isolate);
  }
  isolate->SetContinuationPreservedEmbedderData(value);
}
}  // namespace

AsyncContextFrame::AsyncContextFrame(Lock& js, StorageEntry storageEntry) {
  KJ_IF_SOME(frame, current(js)) {
    // Propagate the storage context of the current frame (if any).
    // If current(js) returns nullptr, we assume we're in the root
    // frame and there is no storage to propagate.
    frame.storage.eraseAll([](const auto& entry) { return entry.key->isDead(); });
    for (auto& entry : frame.storage) {
      storage.insert(entry.clone(js));
    }
  }

  // This case is extremely unlikely to happen but let's handle it anyway
  // just out of an excess of caution.
  if (storageEntry.key->isDead()) return;

  storage.upsert(kj::mv(storageEntry), [](StorageEntry& existing, StorageEntry&& row) mutable {
    existing.value = kj::mv(row.value);
  });
}

AsyncContextFrame::StorageEntry::StorageEntry(kj::Own<StorageKey> key, Value value)
    : key(kj::mv(key)), value(kj::mv(value)) {}

AsyncContextFrame::StorageEntry AsyncContextFrame::StorageEntry::clone(Lock& js) {
  return StorageEntry(kj::addRef(*key), value.addRef(js));
}

kj::Maybe<AsyncContextFrame&> AsyncContextFrame::current(Lock& js) {
  return current(js.v8Isolate);
}

kj::Maybe<Ref<AsyncContextFrame>> AsyncContextFrame::currentRef(Lock& js) {
  return jsg::AsyncContextFrame::current(js).map([](jsg::AsyncContextFrame& frame) {
    return frame.addRef();
  });
}

kj::Maybe<AsyncContextFrame&> AsyncContextFrame::current(v8::Isolate* isolate) {
  auto value = isolate->GetContinuationPreservedEmbedderData();
  KJ_IF_SOME(wrappable, Wrappable::tryUnwrapOpaque(isolate, value)) {
    AsyncContextFrame* frame = dynamic_cast<AsyncContextFrame*>(&wrappable);
    KJ_ASSERT(frame != nullptr);
    return *frame;
  }
  return kj::none;
}

Ref<AsyncContextFrame> AsyncContextFrame::create(Lock& js, StorageEntry storageEntry) {
  return alloc<AsyncContextFrame>(js, kj::mv(storageEntry));
}

v8::Local<v8::Function> AsyncContextFrame::wrap(
    Lock& js, V8Ref<v8::Function>& fn,
    kj::Maybe<v8::Local<v8::Value>> thisArg) {
  return wrap(js, fn.getHandle(js), thisArg);
}

v8::Local<v8::Function> AsyncContextFrame::wrapSnapshot(Lock& js) {
  return js.wrapReturningFunction(js.v8Context(), JSG_VISITABLE_LAMBDA(
    (frame = AsyncContextFrame::currentRef(js)),
    (frame),
    (Lock& js, const v8::FunctionCallbackInfo<v8::Value>& args) {
      auto context = js.v8Context();
      JSG_REQUIRE(args[0]->IsFunction(), TypeError, "The first argument must be a function");
      auto fn = args[0].As<v8::Function>();
      kj::Vector<v8::Local<v8::Value>> argv(args.Length() - 1);
      for (int n = 1; n < args.Length(); n++) {
        argv.add(args[n]);
      }

      AsyncContextFrame::Scope scope(js, frame);
      return check(fn->Call(context, context->Global(), argv.size(), argv.begin()));
    }
  ));
}

v8::Local<v8::Function> AsyncContextFrame::wrap(
    Lock& js,
    v8::Local<v8::Function> fn,
    kj::Maybe<v8::Local<v8::Value>> thisArg) {
  auto context = js.v8Context();

  return js.wrapReturningFunction(context, JSG_VISITABLE_LAMBDA(
      (
        frame = JSG_THIS,
        thisArg = js.v8Ref(thisArg.orDefault(context->Global())),
        fn = js.v8Ref(fn)
      ),
      (frame, thisArg, fn),
      (Lock& js, const v8::FunctionCallbackInfo<v8::Value>& args) {
    auto function = fn.getHandle(js);
    auto context = js.v8Context();

    kj::Vector<v8::Local<v8::Value>> argv(args.Length());
    for (int n = 0; n < args.Length(); n++) {
      argv.add(args[n]);
    }

    AsyncContextFrame::Scope scope(js, *frame.get());
    return check(function->Call(context, thisArg.getHandle(js), args.Length(), argv.begin()));
  }));
}

v8::Local<v8::Function> AsyncContextFrame::wrapRoot(
    Lock& js,
    v8::Local<v8::Function> fn,
    kj::Maybe<v8::Local<v8::Value>> thisArg) {
  auto context = js.v8Context();

  return js.wrapReturningFunction(context, JSG_VISITABLE_LAMBDA(
      (
        thisArg = js.v8Ref(thisArg.orDefault(context->Global())),
        fn = js.v8Ref(fn)
      ),
      (thisArg, fn),
      (Lock& js, const v8::FunctionCallbackInfo<v8::Value>& args) {
    auto function = fn.getHandle(js);
    auto context = js.v8Context();

    kj::Vector<v8::Local<v8::Value>> argv(args.Length());
    for (int n = 0; n < args.Length(); n++) {
      argv.add(args[n]);
    }

    AsyncContextFrame::Scope scope(js, kj::none);
    return check(function->Call(context, thisArg.getHandle(js), args.Length(), argv.begin()));
  }));
}

kj::Maybe<Value&> AsyncContextFrame::get(StorageKey& key) {
  KJ_ASSERT(!key.isDead());
  storage.eraseAll([](const auto& entry) { return entry.key->isDead(); });
  return storage.find(key).map([](auto& entry) -> Value& { return entry.value; });
}

AsyncContextFrame::Scope::Scope(Lock& js, kj::Maybe<AsyncContextFrame&> resource)
    : Scope(js.v8Isolate, resource) {}

AsyncContextFrame::Scope::Scope(v8::Isolate* ptr, kj::Maybe<AsyncContextFrame&> maybeFrame)
    : isolate(ptr),
      prior(AsyncContextFrame::current(ptr)) {
  maybeSetV8ContinuationContext(isolate, maybeFrame);
}

AsyncContextFrame::Scope::Scope(Lock& js, kj::Maybe<Ref<AsyncContextFrame>>& resource)
    : Scope(js.v8Isolate, resource.map([](Ref<AsyncContextFrame>& frame) -> AsyncContextFrame& {
      return *frame.get();
    })) {}

AsyncContextFrame::Scope::~Scope() noexcept(false) {
  maybeSetV8ContinuationContext(isolate, prior);
}

AsyncContextFrame::StorageScope::StorageScope(
    Lock& js,
    StorageKey& key,
    Value store)
    : frame(AsyncContextFrame::create(js, StorageEntry(kj::addRef(key), kj::mv(store)))),
      scope(js, *frame) {}

v8::Local<v8::Object> AsyncContextFrame::getJSWrapper(v8::Isolate* isolate) {
  KJ_IF_SOME(handle, tryGetHandle(isolate)) {
    return handle;
  }
  return attachOpaqueWrapper(isolate->GetCurrentContext(), true);
}

v8::Local<v8::Object> AsyncContextFrame::getJSWrapper(Lock& js) {
  return getJSWrapper(js.v8Isolate);
}

void AsyncContextFrame::jsgVisitForGc(GcVisitor& visitor) {
  for (auto& entry : storage) {
    visitor.visit(entry.value);
  }
}
}  // namespace workerd::jsg
