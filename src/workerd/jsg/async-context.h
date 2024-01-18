// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#pragma once

#include "jsg.h"
#include <v8.h>

namespace workerd::jsg {

#ifndef V8_ENABLE_CONTINUATION_PRESERVED_EMBEDDER_DATA
#error "V8_ENABLE_CONTINUATION_PRESERVED_EMBEDDER_DATA must be defined"
#endif

// Provides for basic internal async context tracking. Eventually, it is expected that
// this will be provided by V8 assuming that the AsyncContext proposal advances through
// TC-39. For now, however, we implement a model that is similar but not quite identical
// to that implemented by Node.js.
//
// At any point in time when JavaScript is running, there is a current "Async Context Frame",
// within which any number of "async resources" can be created. The term "resource" here
// comes from Node.js (which really doesn't take the time to define it properly). Conceptually,
// an "async resource" is some Thing that generates asynchronous activity over time (either
// once or repeatedly). For instance, a timer is an async resource that invokes a callback
// after a certain period of time elapses; a promise is an async resource that may trigger
// scheduling of a microtask at some point in the future, and so forth. Whether or not
// "resource" is the best term to use to describe these, it's what we have because our
// intent here is to stay aligned with Node.js' model as closely as possible.
//
// Every async resource maintains a reference to the Async Context Frame that was current
// at the moment the resource is created.
//
// Frames form a logical stack. The default frame is the Root. We "enter" a frame by pushing
// it onto to top of the stack (making it "current"), then perform some action within that
// frame, then "exit" by popping it back off the stack. The Root is associated with the
// Isolate itself such that every isolate always has at least one frame logically on the stack
// at all times. In Node.js terms, the "Async Context Frame" would be most closely aligned
// with the concept of an "execution context" or "execution scope".
//
// Every Frame has a storage context. The current frame determines the currently active
// storage context. So, for instance, when we start executing, the Root Frame's storage
// context is active. When a timeout elapses and a timer is going to fire, we enter the
// timer's Frame which makes that frame's storage context active. Once the timer
// callback has completed, we return back to the Root frame and storage context.
//
// All frames (except for the Root) are created within the scope of a parent, which by
// default is whichever frame is current when the new frame is created. When the new frame
// is created, it inherits a copy storage context of the parent.
//
// To implement all of this, however, we depend largely on an obscure v8 API on the
// v8::Context object called SetContinuationPreservedEmbedderData and
// GetContinuationPreservedEmbedderData. An AsyncContextFrame is a Wrappable because
// because instances of AsyncContextFrame are set as the continuation-preserved embedder
// data and that API requires a JS value.
//
// AsyncContextFrame::current() returns the current frame or nullptr. Returning nullptr
// implies that we are in the "root" frame.
//
// AsyncContextFrame::StorageScope is created on stack to create a new frame and set
// a stored value in the storage context before entering it.
//
// AsyncContextFrame::Scope is created on the stack to temporarily enter an existing
// frame.
//
// AsyncContextFrame::StorageKey is used to define a storage cell within the storage
// context.
class AsyncContextFrame final: public Wrappable {
public:
  // An opaque key that identifies an async-local storage cell within the frame.
  class StorageKey: public kj::Refcounted {
  public:
    StorageKey() : hash(kj::hashCode(this)) {}
    KJ_DISALLOW_COPY_AND_MOVE(StorageKey);

    // The owner of the key should reset it when it goes away.
    // The StorageKey is typically owned by an instance of AsyncLocalStorage (see
    // the api/node/async-hooks.h). When the ALS instance is garbage collected, it
    // must call reset to signal that this StorageKey is "dead" and can never be
    // looked up again. Subsequent accesses to a frame will remove dead keys from
    // the frame lazily. The lazy cleanup does mean that values may persist in
    // memory a bit longer so if it proves to be problematic we can make the cleanup
    // a bit more proactive.
    void reset() { dead = true; }
    // TODO(later): We should also evaluate the relatively unlikely case where an
    // ALS is capturing a reference to itself and therefore can never be cleaned up.

    bool isDead() const { return dead; }
    inline uint hashCode() const { return hash; }
    inline bool operator==(const StorageKey& other) const {
      return this == &other;
    }

  private:
    uint hash;
    bool dead = false;
  };

  struct StorageEntry {
    kj::Own<StorageKey> key;
    Value value;

    inline StorageEntry clone(Lock& js) {
      return {
        .key = kj::addRef(*key),
        .value = value.addRef(js)
      };
    }
  };

  AsyncContextFrame(Lock& js, StorageEntry storageEntry);

  inline Ref<AsyncContextFrame> addRef() { return JSG_THIS; }

  // Returns the reference to the AsyncContextFrame currently at the top of the stack, if any.
  static kj::Maybe<AsyncContextFrame&> current(Lock& js);

  // Returns the reference to the AsyncContextFrame currently at the top of the stack, if any.
  static kj::Maybe<AsyncContextFrame&> current(v8::Isolate* isolate);

  // Convenience variation on current() that returns the result wrapped in a Ref for when we
  // need to make sure the frame stays alive.
  static kj::Maybe<Ref<AsyncContextFrame>> currentRef(Lock& js);

  // Create a new AsyncContextFrame. The new frame inherits the storage context of the current
  // frame (if any) and the given StorageEntry is added.
  static Ref<AsyncContextFrame> create(Lock& js, StorageEntry storageEntry);

  // Wraps the given JavaScript function such that whenever the wrapper function is called,
  // the root AsyncContextFrame will be entered.
  static v8::Local<v8::Function> wrapRoot(
      Lock& js, v8::Local<v8::Function> fn,
      kj::Maybe<v8::Local<v8::Value>> thisArg = kj::none);

  // Returns a function that captures the current frame and calls the function passed
  // in as an argument within that captured context. Equivalent to wrapping a function
  // with the signature (cb, ...args) => cb(...args).
  static v8::Local<v8::Function> wrapSnapshot(Lock& js);

  // Associates the given JavaScript function with this AsyncContextFrame, returning
  // a wrapper function that will ensure appropriate propagation of the async context
  // when the wrapper function is called.
  v8::Local<v8::Function> wrap(
      Lock& js, V8Ref<v8::Function>& fn,
      kj::Maybe<v8::Local<v8::Value>> thisArg = kj::none);

  // Associates the given JavaScript function with this AsyncContextFrame, returning
  // a wrapper function that will ensure appropriate propagation of the async context
  // when the wrapper function is called.
  v8::Local<v8::Function> wrap(
      Lock& js, v8::Local<v8::Function> fn,
      kj::Maybe<v8::Local<v8::Value>> thisArg = kj::none);

  // AsyncContextFrame::Scope makes the given AsyncContextFrame the current in the
  // stack until the scope is destroyed.
  struct Scope {
    v8::Isolate* isolate;
    kj::Maybe<AsyncContextFrame&> prior;
    // If frame is nullptr, the root frame is assumed.
    Scope(Lock& js, kj::Maybe<AsyncContextFrame&> frame = kj::none);
    // If frame is nullptr, the root frame is assumed.
    Scope(v8::Isolate* isolate, kj::Maybe<AsyncContextFrame&> frame = kj::none);
    // If frame is nullptr, the root frame is assumed.
    Scope(Lock& js, kj::Maybe<Ref<AsyncContextFrame>>& frame);
    ~Scope() noexcept(false);
    KJ_DISALLOW_COPY(Scope);
  };

  // Retrieves the value that is associated with the given key.
  kj::Maybe<Value&> get(StorageKey& key);

  // Gets an opaque JavaScript Object wrapper object for this frame. If a wrapper
  // does not currently exist, one is created.
  v8::Local<v8::Object> getJSWrapper(v8::Isolate* isolate);

  // Gets an opaque JavaScript Object wrapper object for this frame. If a wrapper
  // does not currently exist, one is created.
  v8::Local<v8::Object> getJSWrapper(Lock& js);

  // Creates a new AsyncContextFrame with a new value for the given
  // StorageKey and sets that frame as current for as long as the StorageScope
  // is alive.
  struct StorageScope {
    Ref<AsyncContextFrame> frame;
    // Note that the scope here holds a bare ref to the AsyncContextFrame so it
    // is important that these member fields stay in the correct cleanup order.
    Scope scope;

    StorageScope(Lock& js, StorageKey& key, Value store);
    KJ_DISALLOW_COPY(StorageScope);
  };

private:
  struct StorageEntryCallbacks {
    StorageKey& keyForRow(StorageEntry& entry) const {
      return *entry.key;
    }

    bool matches(const StorageEntry& entry, StorageKey& key) const {
      return entry.key.get() == &key;
    }

    uint hashCode(StorageKey& key) const {
      return key.hashCode();
    }
  };

  using Storage = kj::Table<StorageEntry, kj::HashIndex<StorageEntryCallbacks>>;
  Storage storage;

  void jsgVisitForGc(GcVisitor& visitor) override;

  friend struct StorageScope;
  friend class IsolateBase;
};

}  // namespace workerd::jsg
