// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "io-channels.h"

#include <workerd/jsg/jsg.h>

namespace workerd {

class ActorCacheInterface;
class SqliteKv;

kj::Array<kj::byte> serializeV8Value(jsg::Lock& js, kj::StringPtr key, const jsg::JsValue& value);

jsg::JsValue deserializeV8Value(jsg::Lock& js, kj::StringPtr key, kj::ArrayPtr<const kj::byte> buf);

// Object that manages storing "externals" into DO KV storage such that writes appear synchronous
// even when they require waiting for async I/O.
//
// "Externals" are references to external resources (capabilities) stored in the DO's KV storage.
//
// Externals are stored as channel tokens. But, creating a channel token may require asynchronus
// I/O, while storing a value to DO KV storage (under sqlite) is intended to be synchronous. To
// handle this, the value is serialized and stored without the tokens, but with references to a
// list of external channels. Live objects representing those chanels are kept in memory in the
// StoredExternalHandler while we wait for tokens to be created. Reads can be served in the
// meantime by using the live objects. Once the tokens are all obtained, they are written to
// storage and the live objects can be dropped.
//
// While pending externals exist, the current transaction must be held open, and the output gate
// held closed. If we fail to create any of the tokens, the transaction is rolled back and the
// output gate broken, just like any hard storage failure. This makes the writes appear synchronous
// from the application's point of view.
class StoredExternalHandler {
 public:
  explicit StoredExternalHandler(ActorCacheInterface& actorCache, SqliteKv& sqliteKv)
      : actorCache(actorCache),
        sqliteKv(sqliteKv) {}

  // Cancel any outstanding task that might call `putExternals()` on the given key in the future.
  // This must be called whenever the key has been invalidated by a new put or delete.
  void cancelPutExternals(kj::StringPtr key);

  class SyncNestedTransaction;

  class Serializer;
  class Deserializer;

 private:
  ActorCacheInterface& actorCache;
  SqliteKv& sqliteKv;

  struct PendingWrite {
    kj::Vector<kj::Own<IoChannelFactory::TokenizableChannel>> channels;
    kj::Promise<void> writePromise = nullptr;
  };

  struct Tombstone {};

  // Maps keys to the list of pending externals for that key. When a write promise completes it
  // removes itself from the map. If there are then no pending writes, `onEmptyFulfiller` is
  // signaled.
  //
  // Entries in this map may also be `Tombstone`s. This is only relevant when in a nested sync
  // transaction, i.e. `currentSyncTxn` is non-null. A tombstone indicates that, if the nested
  // transaction manages to be committed, we need to cancel writes associated with the key in the
  // parent.
  kj::HashMap<kj::String, kj::OneOf<PendingWrite, Tombstone>> pendingWrites;

  // When `pendingWrites` becomes empty, this should be fulfilled.
  kj::Maybe<kj::Own<kj::PromiseFulfiller<void>>> onEmptyFulfiller;

  // If we're in a nested sync transaction (an instance of `SyncNestedTransaction` exists on the
  // stack) then this points to it.
  kj::Maybe<SyncNestedTransaction&> currentSyncTxn;

  void fulfillIfEmpty();
  kj::Maybe<kj::Array<kj::Own<IoChannelFactory::TokenizableChannel>>> findPendingWriteForRead(
      kj::StringPtr key);
  void cancelAllPendingWrites();
  bool needsTombstone(kj::StringPtr key);

  static StoredExternalHandler& current();
};

// Construct this class on the stack while performing a synchronous nested transaction. It will
// move the pending external writes off to the side while the nested transaction performs its own
// writes, then the sets will be properly merged or canceled when it is known whether the nested
// transaction will be committed or rolled back.
class StoredExternalHandler::SyncNestedTransaction final {
 public:
  explicit SyncNestedTransaction(StoredExternalHandler& handler);
  ~SyncNestedTransaction() noexcept(false);
  KJ_DISALLOW_COPY_AND_MOVE(SyncNestedTransaction);

  // Call if the transaction completed successfully. Otherwise, rollback is assumed.
  void commit();

 private:
  friend class StoredExternalHandler;

  StoredExternalHandler& handler;
  kj::Maybe<SyncNestedTransaction&> parent;

  // Set of writes that were pending before the nested transaction opened. We move these to the
  // side so that if the nested transaction is rolled back we can restore them verbatim. On
  // success, we'll instead cancel any writes that were overwritten by the nested transaction,
  // and then merge the rest.
  kj::HashMap<kj::String, kj::OneOf<PendingWrite, Tombstone>> savedWrites;
};

// ExternalHandler used during serialization of stored values.
class StoredExternalHandler::Serializer final: public jsg::Serializer::ExternalHandler {
 public:
  explicit Serializer(kj::StringPtr key): key(key) {}
  ~Serializer() noexcept(false);  // inserts the pending externals into `handler`

  // Add an external to the list.
  //
  // TEMPORARY: The caller is expected to have called `getTokenMaybeSync()` already, and if the
  // token is available synchronously, then it is serialized directly without using the externals
  // mechanism. This is for backwards-compatibility as we roll out the new externals mechanism, to
  // avoid writing backwards-incompatible data during the rollout.
  //
  // TODO(cleanup): Once rolled out, we should switch to always store tokens using the externals
  //   mechanism. We can remove the second parameter here at that time, and instead have
  //   writeChannel() make the call directly.
  void writeChannel(kj::Own<IoChannelFactory::TokenizableChannel> channel,
      kj::Promise<kj::Array<byte>> tokenPromise);

 private:
  kj::StringPtr key;

  struct State {
    StoredExternalHandler& handler;

    kj::Vector<kj::Own<IoChannelFactory::TokenizableChannel>> channels;
    kj::Vector<kj::Promise<kj::Array<byte>>> tokenPromises;

    explicit State(StoredExternalHandler& handler): handler(handler) {}
  };

  // Initialized when the first external is written.
  kj::Maybe<State> state;

  State& getState();
};

// ExternalHandler used during deserialization of stored values.
class StoredExternalHandler::Deserializer final: public jsg::Deserializer::ExternalHandler {
 public:
  explicit Deserializer(kj::StringPtr key): key(key) {}

  // Read an external. Externals are expected to be read in the same order they were written.
  kj::Own<IoChannelFactory::SubrequestChannel> readSubrequestChannel(IoChannelFactory& factory);
  kj::Own<IoChannelFactory::ActorClassChannel> readActorClassChannel(IoChannelFactory& factory);

  // Throw if we haven't read all channels.
  void assertDone();

 private:
  kj::StringPtr key;

  struct State {
    StoredExternalHandler& handler;

    // If there is an active PendingWrite, we hold a reference to it here. We hold Rc<PendingWrite>
    // so that if *during deserialization* someone performs a new put() on this key, it won't
    // disrupt deserialization, which can continue with the previous value.
    //
    // If there is no active PendingWrite, we hold an array of tokens instead, read directly from
    // storage.
    kj::OneOf<kj::Array<kj::Own<IoChannelFactory::TokenizableChannel>>, kj::Array<kj::Array<byte>>>
        externals;

    // Index of next external to be read.
    uint index = 0;

    explicit State(StoredExternalHandler& handler): handler(handler) {}
  };

  // Initialized when the first external is read.
  kj::Maybe<State> state;

  State& getState();

  kj::OneOf<kj::Own<IoChannelFactory::TokenizableChannel>, kj::ArrayPtr<const byte>>
  readChannelImpl();
};

}  // namespace workerd
