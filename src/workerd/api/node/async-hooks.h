// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#pragma once

#include <workerd/jsg/jsg.h>
#include <workerd/io/compatibility-date.capnp.h>
#include <workerd/jsg/async-context.h>
#include <kj/table.h>

namespace workerd::api::node {

class AsyncLocalStorage final: public jsg::Object {
  // Implements a subset of the Node.js AsyncLocalStorage API.
  //
  // Example:
  //
  //   import * as async_hooks from 'node:async_hooks';
  //   const als = new async_hooks.AsyncLocalStorage();
  //
  //   async function doSomethingAsync() {
  //     await scheduler.wait(100);
  //     console.log(als.getStore()); // 1
  //   }
  //
  //   als.run(1, async () => {
  //     console.log(als.getStore());  // 1
  //     await doSomethingAsync();
  //     console.log(als.getStore());  // 1
  //   });
  //   console.log(als.getStore());  // undefined
public:
  AsyncLocalStorage() : key(kj::refcounted<jsg::AsyncContextFrame::StorageKey>()) {}
  ~AsyncLocalStorage() noexcept(false) { key->reset(); }

  static jsg::Ref<AsyncLocalStorage> constructor(jsg::Lock& js);

  v8::Local<v8::Value> run(jsg::Lock& js,
                           v8::Local<v8::Value> store,
                           v8::Local<v8::Function> callback,
                           jsg::Varargs args);

  v8::Local<v8::Value> exit(jsg::Lock& js,
                           v8::Local<v8::Function> callback,
                           jsg::Varargs args);

  v8::Local<v8::Value> getStore(jsg::Lock& js);

  static v8::Local<v8::Function> bind(jsg::Lock& js, v8::Local<v8::Function> fn);
  // Binds the given function to the current async context frame such that
  // whenever the function is called, the bound frame is entered.

  static v8::Local<v8::Function> snapshot(jsg::Lock& js);
  // Returns a function bound to the current async context frame that calls
  // the function passed to it as the only argument within that frame.
  // Equivalent to AsyncLocalStorage.bind((cb, ...args) => cb(...args)).

  inline void enterWith(jsg::Lock&, v8::Local<v8::Value>) {
    KJ_UNIMPLEMENTED("asyncLocalStorage.enterWith() is not implemented");
  }

  inline void disable(jsg::Lock&) {
    KJ_UNIMPLEMENTED("asyncLocalStorage.disable() is not implemented");
  }

  JSG_RESOURCE_TYPE(AsyncLocalStorage, CompatibilityFlags::Reader flags) {
    JSG_METHOD(run);
    JSG_METHOD(exit);
    JSG_METHOD(getStore);
    JSG_METHOD(enterWith);
    JSG_METHOD(disable);
    JSG_STATIC_METHOD(bind);
    JSG_STATIC_METHOD(snapshot);

    if (flags.getNodeJsCompat()) {
      JSG_TS_OVERRIDE(AsyncLocalStorage<T> {
        getStore(): T | undefined;
        run<R, TArgs extends any[]>(store: T, callback: (...args: TArgs) => R, ...args: TArgs): R;
        exit<R, TArgs extends any[]>(callback: (...args: TArgs) => R, ...args: TArgs): R;
        disable(): void;
        enterWith(store: T): void;
        static bind<Func extends (...args: any[]) => any>(fn: Func): Func;
        static snapshot<R, TArgs extends any[]>() : ((...args: TArgs) => R, ...args: TArgs) => R;
      });
    } else {
      JSG_TS_OVERRIDE(type AsyncLocalStorage = never);
    }
  }

private:
  kj::Own<jsg::AsyncContextFrame::StorageKey> key;
};


class AsyncResource final: public jsg::Object {
  // Note: The AsyncResource class is provided for Node.js backwards compatibility.
  // The class can be replaced entirely for async context tracking using the
  // AsyncLocalStorage.bind() and AsyncLocalStorage.snapshot() APIs.
  //
  // The AsyncResource class is an object that user code can use to define its own
  // async resources for the purpose of storage context propagation. For instance,
  // let's imagine that we have an EventTarget and we want to register two event listeners
  // on it that will share the same AsyncLocalStorage context. We can use AsyncResource
  // to easily define the context and bind multiple event handler functions to it:
  //
  //   const als = new AsyncLocalStorage();
  //   const context = als.run(123, () => new AsyncResource('foo'));
  //   const target = new EventTarget();
  //   target.addEventListener('abc', context.bind(() => console.log(als.getStore())));
  //   target.addEventListener('xyz', context.bind(() => console.log(als.getStore())));
  //   target.addEventListener('bar', () => console.log(als.getStore()));
  //
  // When the 'abc' and 'xyz' events are emitted, their event handlers will print 123
  // to the console. When the 'bar' event is emitted, undefined will be printed.
  //
  // Alternatively, we can use EventTarget's object event handler:
  //
  //   const als = new AsyncLocalStorage();
  //
  //   class MyHandler extends AsyncResource {
  //     constructor() { super('foo'); }
  //     void handleEvent() {
  //       this.runInAsyncScope(() => console.log(als.getStore()));
  //     }
  //   }
  //
  //   const handler = als.run(123, () => new MyHandler());
  //   const target = new EventTarget();
  //   target.addEventListener('abc', handler);
  //   target.addEventListener('xyz', handler);
public:
  struct Options {
    jsg::WontImplement triggerAsyncId;
    // Node.js' API allows user code to create AsyncResource instances within an
    // explicitly specified parent execution context (what we call an "Async Context
    // Frame") that is specified by a numeric ID. We do not track our context frames
    // by ID and always create new AsyncResource instances within the current Async
    // Context Frame. To prevent subtle bugs, we'll throw explicitly if user code
    // tries to set the triggerAsyncId option.

    // Node.js also has an additional `requireManualDestroy` boolean option
    // that we do not implement. We can simply omit it here. There's no risk of
    // bugs or unexpected behavior by doing so.

    JSG_STRUCT_TS_OVERRIDE(type AsyncResourceOptions = never);
    JSG_STRUCT(triggerAsyncId);
  };

  AsyncResource(jsg::Lock& js);

  static jsg::Ref<AsyncResource> constructor(jsg::Lock& js, jsg::Optional<kj::String> type,
                                             jsg::Optional<Options> options = nullptr);
  // While Node.js' API expects the first argument passed to the `new AsyncResource(...)`
  // constructor to be a string specifying the resource type, we do not actually use it
  // for anything. We'll just ignore the value and not store it, but we at least need to
  // accept the argument and validate that it is a string.

  inline jsg::Unimplemented asyncId() { return {}; }
  inline jsg::Unimplemented triggerAsyncId() { return {}; }
  // The Node.js API uses numeric identifiers for all async resources. We do not
  // implement that part of their API. To prevent subtle bugs, we'll throw explicitly.

  static v8::Local<v8::Function> staticBind(
      jsg::Lock& js,
      v8::Local<v8::Function> fn,
      jsg::Optional<kj::String> type,
      jsg::Optional<v8::Local<v8::Value>> thisArg,
      const jsg::TypeHandler<jsg::Ref<AsyncResource>>& handler);

  v8::Local<v8::Function> bind(
      jsg::Lock& js,
      v8::Local<v8::Function> fn,
      jsg::Optional<v8::Local<v8::Value>> thisArg,
      const jsg::TypeHandler<jsg::Ref<AsyncResource>>& handler);
  // Binds the given function to this async context.

  v8::Local<v8::Value> runInAsyncScope(
      jsg::Lock& js,
      v8::Local<v8::Function> fn,
      jsg::Optional<v8::Local<v8::Value>> thisArg,
      jsg::Varargs args);
  // Calls the given function within this async context.

  JSG_RESOURCE_TYPE(AsyncResource, CompatibilityFlags::Reader flags) {
    JSG_STATIC_METHOD_NAMED(bind, staticBind);
    JSG_METHOD(asyncId);
    JSG_METHOD(triggerAsyncId);
    JSG_METHOD(bind);
    JSG_METHOD(runInAsyncScope);

    if (flags.getNodeJsCompat()) {
      JSG_TS_OVERRIDE(interface AsyncResourceOptions {
        triggerAsyncId?: number;
      });

      JSG_TS_OVERRIDE(AsyncResource {
        constructor(type: string, options?: AsyncResourceOptions);
        static bind<Func extends (this: ThisArg, ...args: any[]) => any, ThisArg>(
            fn: Func,
            type?: string,
            thisArg?: ThisArg): Func & { asyncResource: AsyncResource; };
        bind<Func extends (...args: any[]) => any>(
            fn: Func ): Func & { asyncResource: AsyncResource; };
        runInAsyncScope<This, Result>(fn: (this: This, ...args: any[]) => Result, thisArg?: This,
                                      ...args: any[]): Result;
        asyncId(): number;
        triggerAsyncId(): number;
      });
    } else {
      JSG_TS_OVERRIDE(type AsyncResource = never);
    }
  }

  kj::Maybe<jsg::AsyncContextFrame&> getFrame();
  // Returns the jsg::AsyncContextFrame captured when the AsyncResource was created,
  // if any.

private:
  kj::Maybe<jsg::Ref<jsg::AsyncContextFrame>> frame;

  inline void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(frame);
  }
};

class AsyncHooksModule final: public jsg::Object {
  // We have no intention of fully-implementing the Node.js async_hooks module.
  // We provide this because AsyncLocalStorage is exposed via async_hooks in
  // Node.js.
public:

  JSG_RESOURCE_TYPE(AsyncHooksModule, CompatibilityFlags::Reader flags) {
    JSG_NESTED_TYPE(AsyncLocalStorage);
    JSG_NESTED_TYPE(AsyncResource);

    if (flags.getNodeJsCompat()) {
      JSG_TS_ROOT();
      JSG_TS_OVERRIDE(AsyncHooksModule {});
    } else {
      JSG_TS_OVERRIDE(type AsyncHooksModule = never);
    }
  }
};

#define EW_NODE_ASYNCHOOKS_ISOLATE_TYPES       \
    api::node::AsyncHooksModule,               \
    api::node::AsyncResource,                  \
    api::node::AsyncResource::Options,         \
    api::node::AsyncLocalStorage

}  // namespace workerd::api::node
