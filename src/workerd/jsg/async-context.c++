// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#include "async-context.h"

#include "jsg.h"

#include <workerd/jsg/memory.h>

#include <v8.h>

namespace workerd::jsg {

namespace {
// Sets only a frame's wrapper or undefined. Rust JSG (src/rust/jsg/async_context.rs) relies on
// this: it treats any embedder data but undefined as a frame, which agrees with current() only
// while nothing else is ever set.
inline void maybeSetV8ContinuationContext(
    v8::Isolate* isolate, kj::Maybe<AsyncContextFrame&> maybeFrame) {
  v8::Local<v8::Value> value;
  KJ_IF_SOME(frame, maybeFrame) {
    value = frame.getJSWrapper(isolate);
  } else {
    value = v8::Undefined(isolate);
  }
  isolate->SetContinuationPreservedEmbedderData(value);
}
}  // namespace

AsyncContextFrame::AsyncContextFrame(Lock& js, StorageEntry storageEntry)
    : AsyncContextFrame(js, kj::arrayPtr(&storageEntry, 1)) {}

AsyncContextFrame::AsyncContextFrame(Lock& js, kj::ArrayPtr<StorageEntry> storageEntries) {
  KJ_IF_SOME(frame, current(js)) {
    // Propagate the storage context of the current frame (if any).
    // If current(js) returns nullptr, we assume we're in the root
    // frame and there is no storage to propagate.
    frame.eraseDeadStorage();
    KJ_SWITCH_ONEOF(frame.storage) {
      KJ_CASE_ONEOF(entries, InlineStorage) {
        for (auto& slot: entries) {
          KJ_IF_SOME(entry, slot) {
            upsertStorageEntry(entry.clone(js));
          }
        }
      }
      KJ_CASE_ONEOF(table, TableStorage) {
        for (auto& entry: table) {
          upsertStorageEntry(entry.clone(js));
        }
      }
    }
  }

  for (auto& entry: storageEntries) {
    upsertStorageEntry(kj::mv(entry));
  }
}

AsyncContextFrame::StorageEntry::StorageEntry(kj::Arc<StorageKey> key, Value value)
    : key(kj::mv(key)),
      value(kj::mv(value)) {}

AsyncContextFrame::StorageEntry AsyncContextFrame::StorageEntry::clone(Lock& js) {
  return StorageEntry(key.addRef(), value.addRef(js));
}

kj::Maybe<AsyncContextFrame&> AsyncContextFrame::current(Lock& js) {
  return current(js.v8Isolate);
}

kj::Maybe<Ref<AsyncContextFrame>> AsyncContextFrame::currentRef(Lock& js) {
  return currentRef(js.v8Isolate);
}

kj::Maybe<Ref<AsyncContextFrame>> AsyncContextFrame::currentRef(v8::Isolate* isolate) {
  return current(isolate).map([](AsyncContextFrame& frame) { return frame.addRef(); });
}

kj::Maybe<AsyncContextFrame&> AsyncContextFrame::current(v8::Isolate* isolate) {
  auto value = isolate->GetContinuationPreservedEmbedderData();
  return tryUnwrap(isolate, value.As<v8::Value>());
}

kj::Maybe<AsyncContextFrame&> AsyncContextFrame::tryUnwrap(
    v8::Isolate* isolate, v8::Local<v8::Value> value) {
  KJ_IF_SOME(wrappable, Wrappable::tryUnwrapOpaque(isolate, value)) {
    AsyncContextFrame* frame = dynamic_cast<AsyncContextFrame*>(&wrappable);
    KJ_ASSERT(frame != nullptr);
    return *frame;
  }
  return kj::none;
}

Ref<AsyncContextFrame> AsyncContextFrame::create(Lock& js, StorageEntry storageEntry) {
  return js.alloc<AsyncContextFrame>(js, kj::mv(storageEntry));
}

Ref<AsyncContextFrame> AsyncContextFrame::create(
    Lock& js, kj::ArrayPtr<StorageEntry> storageEntries) {
  return js.alloc<AsyncContextFrame>(js, storageEntries);
}

kj::Maybe<Value&> AsyncContextFrame::get(const StorageKey& key) {
  KJ_ASSERT(!key.isDead());
  eraseDeadStorage();
  KJ_SWITCH_ONEOF(storage) {
    KJ_CASE_ONEOF(entries, InlineStorage) {
      for (auto& slot: entries) {
        KJ_IF_SOME(entry, slot) {
          if (entry.key.get() == &key) return entry.value;
        }
      }
      return kj::none;
    }
    KJ_CASE_ONEOF(table, TableStorage) {
      return table.find(key).map([](auto& entry) -> Value& { return entry.value; });
    }
  }
  KJ_UNREACHABLE;
}

void AsyncContextFrame::eraseDeadStorage() {
  KJ_SWITCH_ONEOF(storage) {
    KJ_CASE_ONEOF(entries, InlineStorage) {
      for (auto& slot: entries) {
        KJ_IF_SOME(entry, slot) {
          if (entry.key->isDead()) slot = kj::none;
        }
      }
    }
    KJ_CASE_ONEOF(table, TableStorage) {
      table.eraseAll([](const auto& entry) { return entry.key->isDead(); });
    }
  }
}

void AsyncContextFrame::upsertStorageEntry(StorageEntry entry) {
  if (entry.key->isDead()) return;
  KJ_SWITCH_ONEOF(storage) {
    KJ_CASE_ONEOF(entries, InlineStorage) {
      for (auto& slot: entries) {
        KJ_IF_SOME(existing, slot) {
          if (existing.key.get() == entry.key.get()) {
            existing.value = kj::mv(entry.value);
            return;
          }
        }
      }
      for (auto& slot: entries) {
        if (slot == kj::none) {
          slot = kj::mv(entry);
          return;
        }
      }
      TableStorage table;
      table.reserve(entries.size() + 1);
      for (auto& slot: entries) {
        table.insert(kj::mv(KJ_ASSERT_NONNULL(slot)));
      }
      table.insert(kj::mv(entry));
      storage = kj::mv(table);
      return;
    }
    KJ_CASE_ONEOF(table, TableStorage) {
      table.upsert(kj::mv(entry), [](StorageEntry& existing, StorageEntry&& row) mutable {
        existing.value = kj::mv(row.value);
      });
    }
  }
}

AsyncContextFrame::Scope::Scope(Lock& js, kj::Maybe<AsyncContextFrame&> resource)
    : Scope(js.v8Isolate, resource) {}

AsyncContextFrame::Scope::Scope(v8::Isolate* ptr, kj::Maybe<AsyncContextFrame&> maybeFrame)
    : isolate(ptr),
      prior(AsyncContextFrame::currentRef(ptr)) {
  maybeSetV8ContinuationContext(isolate, maybeFrame);
}

AsyncContextFrame::Scope::Scope(Lock& js, kj::Maybe<Ref<AsyncContextFrame>>& resource)
    : Scope(js.v8Isolate, resource.map([](Ref<AsyncContextFrame>& frame) -> AsyncContextFrame& {
        return *frame.get();
      })) {}

AsyncContextFrame::Scope::~Scope() noexcept(false) {
  maybeSetV8ContinuationContext(isolate,
      prior.map([](Ref<AsyncContextFrame>& frame) -> AsyncContextFrame& { return *frame; }));
}

AsyncContextFrame::StorageScope::StorageScope(Lock& js, kj::Arc<StorageKey> key, Value store)
    : frame(AsyncContextFrame::create(js, StorageEntry(kj::mv(key), kj::mv(store)))),
      scope(js, *frame) {}

AsyncContextFrame::StorageScope::StorageScope(Lock& js, kj::ArrayPtr<StorageEntry> storageEntries)
    : frame(AsyncContextFrame::create(js, storageEntries)),
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
  // Trace stored values instead of holding them strongly. A store can point back to something
  // that holds this frame, such as a Response created inside als.run(). A strong handle in that
  // loop would keep everything in it alive forever.
  KJ_SWITCH_ONEOF(storage) {
    KJ_CASE_ONEOF(entries, InlineStorage) {
      for (auto& slot: entries) {
        KJ_IF_SOME(entry, slot) {
          visitor.visit(entry.value);
        }
      }
    }
    KJ_CASE_ONEOF(table, TableStorage) {
      for (auto& entry: table) {
        visitor.visit(entry.value);
      }
    }
  }
}
}  // namespace workerd::jsg
