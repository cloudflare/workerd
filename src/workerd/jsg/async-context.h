// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#pragma once

#include "jsg.h"
#include <v8.h>

namespace workerd::jsg {

class AsyncContextFrame final: public Wrappable {
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
  // Frames form a stack. The default frame is the Root. We "enter" a frame by pushing it
  // onto to top of the stack (making it "current"), then perform some action within that
  // frame, then "exit" by popping it back off the stack. The Root is associated with the
  // Isolate itself such that every isolate always has at least one frame on the stack at
  // all times. In Node.js terms, the "Async Context Frame" would be most closely aligned
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
  // AsyncContextFrame's are Wrappables because for certain kinds of async resources
  // like promises or JS functions, we have to be able to store a reference to the frame
  // using an opaque private property rather than an internal field or lambda capture.
  // In such cases, we attach an opaque JS wrapper to the frame and use that opaque
  // wrapper to hold the frame reference.
public:
  class StorageKey: public kj::Refcounted {
    // An opaque key that identifies an async-local storage cell within the frame.
  public:
    StorageKey() : hash(kj::hashCode(this)) {}

    void reset() { dead = true; }
    // The owner of the key should reset it when it goes away.
    // The StorageKey is typically owned by an instance of AsyncLocalstorage (see
    // the api/node/async-hooks.h). When the ALS instance is garbage collected, it
    // must call reset to signal that this StorageKey is "dead" and can never be
    // looked up again. Subsequent accesses to a frame will remove dead keys from
    // the frame lazily. The lazy cleanup does mean that values may persist in
    // memory a bit longer so if it proves to be problematic we can make the cleanup
    // a bit more proactive.
    //
    // TODO(later): We should also evaluate the relatively unlikely case where an
    // ALS is capturing a reference to itself and therefore can never be cleaned up.

    bool isDead() const { return dead; }
    inline uint hashCode() const { return hash; }
    inline bool operator==(const StorageKey& other) const {
      return hash == other.hash;
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

  static kj::Maybe<AsyncContextFrame&> current(Lock& js);
  // Returns the reference to the AsyncContextFrame currently at the top of the stack, if any.

  static Ref<AsyncContextFrame> create(Lock& js, StorageEntry storageEntry);
  // Create a new AsyncContextFrame. The new frame inherits the storage context of the current
  // frame (if any) and the given StorageEntry is added.

  static v8::Local<v8::Function> wrapRoot(
      Lock& js, v8::Local<v8::Function> fn,
      kj::Maybe<v8::Local<v8::Value>> thisArg = nullptr);
  // Wraps the given JavaScript function such that whenever the wrapper function is called,
  // the root AsyncContextFrame will be entered.

  v8::Local<v8::Function> wrap(
      Lock& js, V8Ref<v8::Function>& fn,
      kj::Maybe<v8::Local<v8::Value>> thisArg = nullptr);
  v8::Local<v8::Function> wrap(
      Lock& js, v8::Local<v8::Function> fn,
      kj::Maybe<v8::Local<v8::Value>> thisArg = nullptr);
  // Associates the given JavaScript function with this AsyncContextFrame, returning
  // a wrapper function that will ensure appropriate propagation of the async context
  // when the wrapper function is called.

  void attachContext(Lock& js, v8::Local<v8::Promise> promise);
  // Associates the given JavaScript promise with this AsyncContextFrame.

  static kj::Maybe<AsyncContextFrame&> tryGetContext(Lock& js, V8Ref<v8::Promise>& promise);
  static kj::Maybe<AsyncContextFrame&> tryGetContext(Lock& js, v8::Local<v8::Promise> promise);
  // Returns a reference to the AsyncContextFrame that was current when the JS Promise
  // was created. When async context tracking is enabled, this should always return a
  // non-null value.

  struct Scope {
    // AsyncContextFrame::Scope makes the given AsyncContextFrame the current in the
    // stack until the scope is destroyed.
    IsolateBase& isolate;
    Scope(Lock& js, kj::Maybe<AsyncContextFrame&> frame = nullptr);
    Scope(v8::Isolate* isolate, kj::Maybe<AsyncContextFrame&> frame = nullptr);
    // If frame is nullptr, the root frame is assumed.
    ~Scope() noexcept(false);
    KJ_DISALLOW_COPY(Scope);
  };

  kj::Maybe<Value&> get(StorageKey& key);
  // Retrieves the value that is associated with the given key.

  v8::Local<v8::Object> getJSWrapper(Lock& js);
  // Gets an opaque JavaScript Object wrapper object for this frame. If a wrapper
  // does not currently exist, one is created. This wrapper is only used to set a
  // private reference to the frame on JS objects like promises and functions.
  // See the attachContext and wrap functions for details.

  struct StorageScope {
    // Creates a new AsyncContextFrame with a new value for the given
    // StorageKey and sets that frame as current for as long as the StorageScope
    // is alive.
    Ref<AsyncContextFrame> frame;
    Scope scope;
    // Note that the scope here holds a bare ref to the AsyncContextFrame so it
    // is important that these member fields stay in the correct cleanup order.

    StorageScope(Lock& js, StorageKey& key, Value store);
    KJ_DISALLOW_COPY(StorageScope);
  };

private:
  struct StorageEntryCallbacks {
    StorageKey& keyForRow(StorageEntry& entry) const {
      return *entry.key;
    }

    bool matches(const StorageEntry& entry, StorageKey& key) const {
      return entry.key->hashCode() == key.hashCode();
    }

    uint hashCode(StorageKey& key) const {
      return key.hashCode();
    }
  };

  using Storage = kj::Table<StorageEntry, kj::HashIndex<StorageEntryCallbacks>>;
  Storage storage;

  IsolateBase& isolate;

  void jsgVisitForGc(GcVisitor& visitor) override;

  friend struct StorageScope;
  friend class IsolateBase;
};

}  // namespace workerd::jsg
