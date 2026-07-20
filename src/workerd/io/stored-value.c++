// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "stored-value.h"

#include "io-context.h"

#include <workerd/util/sqlite-kv.h>

namespace workerd {

namespace {

// Return the id of the current actor (or the empty string if there is no current actor).
kj::Maybe<kj::String> getCurrentActorId() {
  KJ_IF_SOME(ioContext, IoContext::tryCurrent()) {
    KJ_IF_SOME(actor, ioContext.getActor()) {
      KJ_SWITCH_ONEOF(actor.getId()) {
        KJ_CASE_ONEOF(s, kj::String) {
          return kj::heapString(s);
        }
        KJ_CASE_ONEOF(actorId, kj::Own<ActorIdFactory::ActorId>) {
          return actorId->toString();
        }
      }
      KJ_UNREACHABLE;
    }
  }
  return kj::none;
}

}  // namespace

kj::Array<kj::byte> serializeV8Value(jsg::Lock& js, kj::StringPtr key, const jsg::JsValue& value) {
  StoredExternalHandler::Serializer externalHandler(key);
  jsg::Serializer serializer(js,
      jsg::Serializer::Options{
        .version = 15,
        .omitHeader = false,
        .externalHandler = externalHandler,
      });
  serializer.write(js, value);
  auto released = serializer.release();
  return kj::mv(released.data);
}

jsg::JsValue deserializeV8Value(
    jsg::Lock& js, kj::StringPtr key, kj::ArrayPtr<const kj::byte> buf) {

  KJ_ASSERT(buf.size() > 0, "unexpectedly empty value buffer", key);
  try {
    // The js.tryCatch will handle the normal exception path. We wrap this in an
    // additional try/catch in case the js.tryCatch hits an exception that is
    // terminal for the isolate, causing exception to be rethrown, in which case
    // we throw a kj::Exception wrapping a jsg.Error.
    return js.tryCatch([&]() -> jsg::JsValue {
      StoredExternalHandler::Deserializer externalHandler(key);
      jsg::Deserializer::Options options{
        .externalHandler = externalHandler,
      };
      if (buf[0] != 0xFF) {
        // When Durable Objects was first released, it did not properly write headers when serializing
        // to storage. If we find that the header is missing (as indicated by the first byte not being
        // 0xFF), it's safe to assume that the data was written at the only serialization version we
        // used during that early time period, so we explicitly set that version here.
        options.version = 13;
        options.readHeader = false;
      }

      jsg::Deserializer deserializer(js, buf, kj::none, kj::none, options);

      auto result = deserializer.readValue(js);
      externalHandler.assertDone();
      return result;
    }, [&](jsg::Value&& exception) mutable -> jsg::JsValue {
      // If we do hit a deserialization error, we log information that will be helpful in
      // understanding the problem but that won't leak too much about the customer's data. We
      // include the key (to help find the data in the database if it hasn't been deleted), the
      // length of the value, and the first three bytes of the value (which is just the v8-internal
      // version header and the tag that indicates the type of the value, but not its contents).
      kj::String actorId = getCurrentActorId().orDefault([]() { return kj::String(); });
      KJ_FAIL_ASSERT("actor storage deserialization failed", "failed to deserialize stored value",
          actorId, exception.getHandle(js), key, buf.size(),
          buf.first(std::min(static_cast<size_t>(3), buf.size())));
    });
  } catch (jsg::JsExceptionThrown&) {
    // We can occasionally hit an isolate termination here -- we prefix the error with jsg to avoid
    // counting it against our internal storage error metrics but also throw a KJ exception rather
    // than a jsExceptionThrown error to avoid confusing the normal termination handling code.
    // We don't expect users to ever actually see this error.
    JSG_FAIL_REQUIRE(Error,
        "isolate terminated while deserializing value from Durable Object "
        "storage; contact us if you're wondering why you're seeing this");
  }
}

void StoredExternalHandler::cancelAllPendingWrites() {
  // cancelAllPendingWrites() is called on rollback, so we actually want to cancel tombstones too.
  // We can just clean the map.
  pendingWrites.clear();
}

bool StoredExternalHandler::needsTombstone(kj::StringPtr key) {
  kj::Maybe<SyncNestedTransaction&> maybeTxn = currentSyncTxn;
  for (;;) {
    auto& txn = KJ_UNWRAP_OR(maybeTxn, break);
    if (txn.savedWrites.find(key) != kj::none) {
      // Key exists in a parent, so we need a tombstone if overwritten in children.
      return true;
    }
    maybeTxn = txn.parent;
  }
  return false;
}

void StoredExternalHandler::cancelPutExternals(kj::StringPtr key) {
  if (needsTombstone(key)) {
    pendingWrites.upsert(key.clone(), Tombstone(),
        [](auto& existing, auto&& replacement) { existing = kj::mv(replacement); });
  } else {
    pendingWrites.erase(key);
    fulfillIfEmpty();
  }
}

StoredExternalHandler::SyncNestedTransaction::SyncNestedTransaction(StoredExternalHandler& handler)
    : handler(handler),
      parent(handler.currentSyncTxn),
      savedWrites(kj::mv(handler.pendingWrites)) {
  handler.currentSyncTxn = this;
}

StoredExternalHandler::SyncNestedTransaction::~SyncNestedTransaction() noexcept(false) {
  // Cancel pending writes if they weren't already committed. (If commit() was called, we already
  // merged pendingWrites into our own savedWrites.)
  handler.cancelAllPendingWrites();

  // Restore handler state.
  handler.pendingWrites = kj::mv(savedWrites);
  handler.currentSyncTxn = parent;

  handler.fulfillIfEmpty();
}

void StoredExternalHandler::SyncNestedTransaction::commit() {
  // Merge all pending writes into the parent set.
  for (auto& entry: handler.pendingWrites) {
    if (parent == kj::none && entry.value.is<Tombstone>()) {
      // Parent is the root transaction, tombstones not needed anymore, just erase the entry.
      savedWrites.erase(entry.key);
    } else {
      // In all other cases, just merge.
      savedWrites.upsert(kj::mv(entry.key), kj::mv(entry.value),
          [](auto& existing, auto&& replacement) { existing = kj::mv(replacement); });
    }
  }
  handler.pendingWrites.clear();
}

// Check if it's time to fulfill `onEmptyFulfiller` and if so, do so.
void StoredExternalHandler::fulfillIfEmpty() {
  if (currentSyncTxn != kj::none) {
    // No need to fulfill yet, we'll do it when the nested transactions unwind.
    return;
  }

  if (pendingWrites.size() > 0) {
    // Not empty yet. Note that the top-level pendingWrites never contains tombstones so we don't
    // have to check for those.
    return;
  }

  KJ_IF_SOME(f, onEmptyFulfiller) {
    f->fulfill();
    onEmptyFulfiller = kj::none;
  }
}

// If there is a pending write for the given key, return its live channel objects. This is used
// when the app tries to read the key back before it is finished writing.
kj::Maybe<kj::Array<kj::Own<IoChannelFactory::TokenizableChannel>>> StoredExternalHandler::
    findPendingWriteForRead(kj::StringPtr key) {
  // Loop up the sync transaction stack to find a matching key.
  decltype(pendingWrites)* nextMap = &pendingWrites;
  kj::Maybe<SyncNestedTransaction&> nextTxn = currentSyncTxn;
  for (;;) {
    KJ_IF_SOME(pending, nextMap->find(key)) {
      KJ_SWITCH_ONEOF(pending) {
        KJ_CASE_ONEOF(write, PendingWrite) {
          return KJ_MAP(channel, write.channels) { return kj::addRef(*channel); };
        }
        KJ_CASE_ONEOF(_, Tombstone) {
          // In the current transaction, this key has been overwritten with something that has
          // no channels.
          return kj::Array<kj::Own<IoChannelFactory::TokenizableChannel>>();
        }
      }
    }

    KJ_IF_SOME(txn, nextTxn) {
      nextMap = &txn.savedWrites;
      nextTxn = txn.parent;
    } else {
      break;
    }
  }

  return kj::none;
}

StoredExternalHandler& StoredExternalHandler::current() {
  // TODO(cleanup): It's a bit ugly that we're plucking the StoredExternalHandler out of the
  //   thread-local IoContext when needed. It really ought to be passed in by the caller of
  //   serializeV8Value() or deserializeV8Value(). However, stringing it through to all the call
  //   sites would be tedious. This tedium will probably be reduced if and when the legacy storage
  //   backend is removed.

  auto& actor = KJ_REQUIRE_NONNULL(
      IoContext::current().getActor(), "serializing/deserializing storage outside of an actor?");
  return actor.getOrCreateStoredExternalHandler();
}

StoredExternalHandler::Serializer::~Serializer() noexcept(false) {
  KJ_IF_SOME(state, this->state) {
    PendingWrite pendingWrite;

    pendingWrite.channels = kj::mv(state.channels);

    pendingWrite.writePromise =
        kj::joinPromisesFailFast(state.tokenPromises.releaseAsArray())
            .then([key = key.clone(), &handler = state.handler](kj::Array<kj::Array<byte>> tokens) {
      // We can't possibly have a sync transaction open at this point because we're in an async
      // continuation. Hence we can assume `pendingWrites` has the final merged set of writes.
      KJ_ASSERT(handler.currentSyncTxn == kj::none);

      // Write the tokens to storage.
      handler.sqliteKv.putExternals(key, kj::mv(tokens));

      // HACK: We're about to erase ourselves from the map, but this would result in
      // self-cancellation. To avoid that, detach this promise first. Since we know the promise
      // is about to be done, detaching it should be safe.
      auto& entry = KJ_ASSERT_NONNULL(handler.pendingWrites.findEntry(key));
      auto& pendingWrite = KJ_ASSERT_NONNULL(entry.value.tryGet<PendingWrite>());
      pendingWrite.writePromise.detach([](kj::Exception&& e) {});

      // Erase ourselves from the map.
      handler.pendingWrites.erase(entry);

      // If that was the last pending write, fulfill the on-empty fulfiller.
      handler.fulfillIfEmpty();
    }).eagerlyEvaluate([&handler = state.handler](kj::Exception&& e) {
      KJ_IF_SOME(f, handler.onEmptyFulfiller) {
        f->reject(e.clone());
        handler.onEmptyFulfiller = kj::none;
      }
    });

    state.handler.pendingWrites.upsert(key.clone(), kj::mv(pendingWrite),
        [](auto& existing, auto&& replacement) { existing = kj::mv(replacement); });

    // If this was the first pending write, arrange to block the transaction until there are no
    // more writes.
    if (state.handler.onEmptyFulfiller == kj::none) {
      auto paf = kj::newPromiseAndFulfiller<void>();
      state.handler.onEmptyFulfiller = kj::mv(paf.fulfiller);
      state.handler.actorCache.blockTransaction(
          paf.promise.attach(kj::defer([&handler = state.handler]() {
        // If handler.pendingWrites is non-empty here, then either one of the writes failed or
        // the promise was canceled due to a rollback. Either way, we should cancel all pending
        // writes.
        handler.cancelAllPendingWrites();
      })));
    }
  } else {
    // We didn't store any externals with this put, but we need to cancel any pending write
    // from a previous put.
    KJ_IF_SOME(ioctx, IoContext::tryCurrent()) {
      KJ_IF_SOME(actor, ioctx.getActor()) {
        KJ_IF_SOME(handler, actor.getStoredExternalHandler()) {
          handler.cancelPutExternals(key);
        }
      }
    }
  }
}

void StoredExternalHandler::Serializer::writeChannel(
    kj::Own<IoChannelFactory::TokenizableChannel> channel,
    kj::Promise<kj::Array<byte>> tokenPromise) {
  State& state = getState();
  state.channels.add(kj::mv(channel));
  state.tokenPromises.add(kj::mv(tokenPromise));
}

StoredExternalHandler::Serializer::State& StoredExternalHandler::Serializer::getState() {
  KJ_IF_SOME(s, this->state) {
    return s;
  } else {
    return this->state.emplace(StoredExternalHandler::current());
  }
}

kj::Own<IoChannelFactory::SubrequestChannel> StoredExternalHandler::Deserializer::
    readSubrequestChannel(IoChannelFactory& factory) {
  KJ_SWITCH_ONEOF(readChannelImpl()) {
    KJ_CASE_ONEOF(channel, kj::Own<IoChannelFactory::TokenizableChannel>) {
      return kj::addRef(
          KJ_REQUIRE_NONNULL(kj::tryDowncast<IoChannelFactory::SubrequestChannel>(*channel)));
    }
    KJ_CASE_ONEOF(token, kj::ArrayPtr<const byte>) {
      return factory.subrequestChannelFromToken(
          IoChannelFactory::ChannelTokenUsage::STORAGE, token);
    }
  }
  KJ_UNREACHABLE;
}

kj::Own<IoChannelFactory::ActorClassChannel> StoredExternalHandler::Deserializer::
    readActorClassChannel(IoChannelFactory& factory) {
  KJ_SWITCH_ONEOF(readChannelImpl()) {
    KJ_CASE_ONEOF(channel, kj::Own<IoChannelFactory::TokenizableChannel>) {
      return kj::addRef(
          KJ_REQUIRE_NONNULL(kj::tryDowncast<IoChannelFactory::ActorClassChannel>(*channel),
              "serialized value doesn't match external type"));
    }
    KJ_CASE_ONEOF(token, kj::ArrayPtr<const byte>) {
      return factory.actorClassFromToken(IoChannelFactory::ChannelTokenUsage::STORAGE, token);
    }
  }
  KJ_UNREACHABLE;
}

kj::Own<IoChannelFactory::RpcChannel> StoredExternalHandler::Deserializer::readRpcChannel(
    IoChannelFactory& factory) {
  KJ_SWITCH_ONEOF(readChannelImpl()) {
    KJ_CASE_ONEOF(channel, kj::Own<IoChannelFactory::TokenizableChannel>) {
      return kj::addRef(KJ_REQUIRE_NONNULL(kj::tryDowncast<IoChannelFactory::RpcChannel>(*channel),
          "serialized value doesn't match external type"));
    }
    KJ_CASE_ONEOF(token, kj::ArrayPtr<const byte>) {
      return factory.rpcChannelFromToken(IoChannelFactory::ChannelTokenUsage::STORAGE, token);
    }
  }
  KJ_UNREACHABLE;
}

StoredExternalHandler::Deserializer::State& StoredExternalHandler::Deserializer::getState() {
  KJ_IF_SOME(s, this->state) {
    return s;
  } else {
    auto& state = this->state.emplace(StoredExternalHandler::current());

    // When first initializing the state, acquire the externals table.
    KJ_IF_SOME(pending, state.handler.findPendingWriteForRead(key)) {
      // A recent write to this key is still pending. Use the associated in-memory channels.
      state.externals = kj::mv(pending);
    } else {
      // Read the tokens from the externals table in the database.
      state.externals = state.handler.sqliteKv.getExternals(key);
    }

    return state;
  }
}

kj::OneOf<kj::Own<IoChannelFactory::TokenizableChannel>, kj::ArrayPtr<const byte>>
StoredExternalHandler::Deserializer::readChannelImpl() {
  auto& state = getState();
  uint idx = state.index++;

  KJ_SWITCH_ONEOF(state.externals) {
    KJ_CASE_ONEOF(channels, kj::Array<kj::Own<IoChannelFactory::TokenizableChannel>>) {
      KJ_REQUIRE(idx < channels.size(), "serialized value doesn't match pending externals?");
      return kj::addRef(*channels[idx]);
    }
    KJ_CASE_ONEOF(tokens, kj::Array<kj::Array<byte>>) {
      KJ_REQUIRE(idx < tokens.size(), "serialized value doesn't match stored externals?");
      return tokens[idx].asPtr().asConst();
    }
  }
  KJ_UNREACHABLE;
}

void StoredExternalHandler::Deserializer::assertDone() {
  KJ_IF_SOME(s, state) {
    KJ_SWITCH_ONEOF(s.externals) {
      KJ_CASE_ONEOF(channels, kj::Array<kj::Own<IoChannelFactory::TokenizableChannel>>) {
        KJ_REQUIRE(s.index == channels.size());
      }
      KJ_CASE_ONEOF(tokens, kj::Array<kj::Array<byte>>) {
        KJ_REQUIRE(s.index == tokens.size());
      }
    }
  }
}

}  // namespace workerd
