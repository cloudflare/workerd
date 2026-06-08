#include "io-channels.h"

#include <workerd/io/worker-interface.h>
#include <workerd/io/worker.h>

namespace workerd {

kj::Promise<kj::Array<byte>> IoChannelFactory::TokenizableChannel::getToken(
    ChannelTokenUsage usage) {
  KJ_SWITCH_ONEOF(getTokenMaybeSync(usage)) {
    KJ_CASE_ONEOF(token, kj::Array<byte>) {
      return kj::mv(token);
    }
    KJ_CASE_ONEOF(promise, kj::Promise<kj::Array<byte>>) {
      return kj::mv(promise);
    }
  }
  KJ_UNREACHABLE;
}

kj::Own<IoChannelFactory::SubrequestChannel> IoChannelFactory::subrequestChannelFromToken(
    ChannelTokenUsage usage, kj::ArrayPtr<const byte> token) {
  JSG_FAIL_REQUIRE(DOMDataCloneError, "This Worker is not able to deserialize ServiceStubs.");
}

kj::Own<IoChannelFactory::ActorClassChannel> IoChannelFactory::actorClassFromToken(
    ChannelTokenUsage usage, kj::ArrayPtr<const byte> token) {
  JSG_FAIL_REQUIRE(
      DOMDataCloneError, "This Worker is not able to deserialize Durable Object class stubs.");
}

kj::Own<IoChannelFactory::RpcChannel> IoChannelFactory::rpcChannelFromToken(
    ChannelTokenUsage usage, kj::ArrayPtr<const byte> token) {
  JSG_FAIL_REQUIRE(DOMDataCloneError, "This Worker is not able to deserialize RpcStubs.");
}

kj::Own<IoChannelFactory::SubrequestChannel> IoChannelFactory::
    makeRestoredSubrequestChannelResolved(kj::Own<SelfTokenFactory> selfTokenFactory,
        Frankenvalue restoreParams,
        kj::Own<SubrequestChannel> inner) {
  KJ_UNIMPLEMENTED("This runtime doesn't support persistent ServiceStubs.");
}

kj::Own<IoChannelFactory::RpcChannel> IoChannelFactory::makeRestoredRpcChannelResolved(
    kj::Own<SelfTokenFactory> selfTokenFactory, Frankenvalue restoreParams) {
  KJ_UNIMPLEMENTED("This runtime doesn't support persistent RpcStubs.");
}

namespace {

template <typename ChannelType>
class PromisedTokenizableChannel: public ChannelType {
 public:
  PromisedTokenizableChannel(kj::Promise<kj::Own<ChannelType>> promise)
      : readyPromise(waitForResolution(kj::mv(promise)).fork()) {}

  void requireAllowsTransfer() override {
    // PromisedTokenizableChannel is used for channels initialized from a promised channel token.
    // A channel created from a channel token should always support transfer, via channel tokens.
  }

  kj::OneOf<kj::Array<byte>, kj::Promise<kj::Array<byte>>> getTokenMaybeSync(
      IoChannelFactory::ChannelTokenUsage usage) override {
    KJ_IF_SOME(channel, inner) {
      return channel->getTokenMaybeSync(usage);
    } else {
      return readyPromise.addBranch().then([this, usage]() -> kj::Promise<kj::Array<byte>> {
        KJ_SWITCH_ONEOF(KJ_ASSERT_NONNULL(inner)->getTokenMaybeSync(usage)) {
          KJ_CASE_ONEOF(token, kj::Array<byte>) {
            return kj::mv(token);
          }
          KJ_CASE_ONEOF(promise, kj::Promise<kj::Array<byte>>) {
            return kj::mv(promise);
          }
        }
        KJ_UNREACHABLE;
      });
    }
  }

  kj::OneOf<kj::Own<IoChannelFactory::TokenizableChannel>,
      kj::Promise<kj::Own<IoChannelFactory::TokenizableChannel>>>
  getResolved() override {
    KJ_IF_SOME(channel, inner) {
      return kj::addRef<IoChannelFactory::TokenizableChannel>(*channel);
    } else {
      return readyPromise.addBranch().then([this]() mutable {
        return kj::addRef<IoChannelFactory::TokenizableChannel>(*KJ_ASSERT_NONNULL(inner));
      });
    }
  }

 protected:
  kj::Maybe<kj::Own<ChannelType>> inner;
  kj::ForkedPromise<void> readyPromise;

  kj::Promise<void> waitForResolution(kj::Promise<kj::Own<ChannelType>> promise) {
    kj::Own<IoChannelFactory::TokenizableChannel> resolution = co_await promise;

    KJ_SWITCH_ONEOF(resolution->getResolved()) {
      KJ_CASE_ONEOF(channel, kj::Own<IoChannelFactory::TokenizableChannel>) {
        inner = channel.template downcast<ChannelType>();
        co_return;
      }
      KJ_CASE_ONEOF(deeperPromise, kj::Promise<kj::Own<IoChannelFactory::TokenizableChannel>>) {
        // Promise resolved to another promise, wait for it too.
        //
        // Note that a promise returned by `getResolved()` will always itself resolve to a
        // fully-resolved channel object, so we don't need to loop here.
        inner = (co_await deeperPromise).template downcast<ChannelType>();
      }
    }
  }
};

class PromisedSubrequestChannel final
    : public PromisedTokenizableChannel<IoChannelFactory::SubrequestChannel> {
 public:
  using PromisedTokenizableChannel::PromisedTokenizableChannel;

  kj::Own<WorkerInterface> startRequest(IoChannelFactory::SubrequestMetadata metadata) override {
    KJ_IF_SOME(channel, inner) {
      return channel->startRequest(kj::mv(metadata));
    } else {
      return newPromisedWorkerInterface(readyPromise.addBranch().then(
          [self = addRefToThis(), metadata = kj::mv(metadata)]() mutable {
        return KJ_ASSERT_NONNULL(self->inner)->startRequest(kj::mv(metadata));
      }));
    }
  }
};

class PromisedActorClassChannel final
    : public PromisedTokenizableChannel<IoChannelFactory::ActorClassChannel> {
 public:
  using PromisedTokenizableChannel::PromisedTokenizableChannel;
};

class PromisedRpcChannel final: public PromisedTokenizableChannel<IoChannelFactory::RpcChannel> {
 public:
  using PromisedTokenizableChannel::PromisedTokenizableChannel;

  Session restore() override {
    KJ_IF_SOME(channel, inner) {
      return channel->restore();
    } else {
      auto splitPromise = readyPromise.addBranch()
                              .then([this]() {
        auto innerRestore = KJ_ASSERT_NONNULL(inner)->restore();
        return kj::tuple(kj::mv(innerRestore.cap), kj::mv(innerRestore.task));
      }).split();
      return {
        .cap = kj::mv(kj::get<0>(splitPromise)),
        .task = kj::mv(kj::get<1>(splitPromise)),
      };
    }
  }
};

kj::OneOf<kj::Own<Frankenvalue::CapTableEntry>, kj::Promise<kj::Own<Frankenvalue::CapTableEntry>>>
resolveCap(kj::Own<Frankenvalue::CapTableEntry> cap) {
  KJ_IF_SOME(typed, kj::tryDowncast<IoChannelFactory::TokenizableChannel>(*cap)) {
    KJ_SWITCH_ONEOF(typed.getResolved()) {
      KJ_CASE_ONEOF(channel, kj::Own<IoChannelFactory::TokenizableChannel>) {
        return kj::implicitCast<kj::Own<Frankenvalue::CapTableEntry>>(kj::mv(channel));
      }
      KJ_CASE_ONEOF(promise, kj::Promise<kj::Own<IoChannelFactory::TokenizableChannel>>) {
        return promise.then([](kj::Own<IoChannelFactory::TokenizableChannel> channel) {
          return kj::implicitCast<kj::Own<Frankenvalue::CapTableEntry>>(kj::mv(channel));
        });
      }
    }
    KJ_UNREACHABLE;
  } else {
    auto& ref = *cap;
    KJ_FAIL_ASSERT("unknown type in Frankenvalue", typeid(ref).name());
  }
}

}  // namespace

kj::Own<IoChannelFactory::SubrequestChannel> IoChannelFactory::getSubrequestChannel(
    uint channel, kj::Maybe<Frankenvalue> props, kj::Maybe<VersionRequest> versionRequest) {
  KJ_IF_SOME(p, props) {
    KJ_IF_SOME(promise, p.resolveCaps(resolveCap)) {
      return kj::refcounted<PromisedSubrequestChannel>(
          promise.then([this, self = addRef(), channel, props = kj::mv(p),
                           versionRequest = kj::mv(versionRequest)]() mutable {
        return getSubrequestChannelResolved(channel, kj::mv(props), kj::mv(versionRequest));
      }));
    }
  }
  return getSubrequestChannelResolved(channel, kj::mv(props), kj::mv(versionRequest));
}

kj::Own<IoChannelFactory::ActorClassChannel> IoChannelFactory::getActorClass(
    uint channel, kj::Maybe<Frankenvalue> props) {
  KJ_IF_SOME(p, props) {
    KJ_IF_SOME(promise, p.resolveCaps(resolveCap)) {
      return kj::refcounted<PromisedActorClassChannel>(
          promise.then([this, self = addRef(), channel, props = kj::mv(p)]() mutable {
        return getActorClassResolved(channel, kj::mv(props));
      }));
    }
  }
  return getActorClassResolved(channel, kj::mv(props));
}

kj::Own<IoChannelFactory::SubrequestChannel> IoChannelFactory::makeRestoredSubrequestChannel(
    kj::Own<SelfTokenFactory> selfTokenFactory,
    Frankenvalue restoreParams,
    kj::Own<SubrequestChannel> inner) {
  // Note that `inner` doesn't need to be resolved since it's only used to forward requests.
  // So, the only thing we might have to wait for is `restoreParams`. Which is good as otherwise
  // this method would get a lot more complicated!

  KJ_IF_SOME(promise, restoreParams.resolveCaps(resolveCap)) {
    return kj::refcounted<PromisedSubrequestChannel>(
        promise.then([this, self = addRef(), selfTokenFactory = kj::mv(selfTokenFactory),
                         restoreParams = kj::mv(restoreParams), inner = kj::mv(inner)]() mutable {
      return makeRestoredSubrequestChannelResolved(
          kj::mv(selfTokenFactory), kj::mv(restoreParams), kj::mv(inner));
    }));
  }

  return makeRestoredSubrequestChannelResolved(
      kj::mv(selfTokenFactory), kj::mv(restoreParams), kj::mv(inner));
}

kj::Own<IoChannelFactory::RpcChannel> IoChannelFactory::makeRestoredRpcChannel(
    kj::Own<SelfTokenFactory> selfTokenFactory, Frankenvalue restoreParams) {
  KJ_IF_SOME(promise, restoreParams.resolveCaps(resolveCap)) {
    return kj::refcounted<PromisedRpcChannel>(
        promise.then([this, self = addRef(), selfTokenFactory = kj::mv(selfTokenFactory),
                         restoreParams = kj::mv(restoreParams)]() mutable {
      return makeRestoredRpcChannelResolved(kj::mv(selfTokenFactory), kj::mv(restoreParams));
    }));
  }

  return makeRestoredRpcChannelResolved(kj::mv(selfTokenFactory), kj::mv(restoreParams));
}

kj::Own<IoChannelFactory::SubrequestChannel> WorkerStubChannel::getEntrypoint(
    kj::Maybe<kj::String> name, Frankenvalue props, kj::Maybe<ResourceLimits> limits) {
  KJ_IF_SOME(promise, props.resolveCaps(resolveCap)) {
    return kj::refcounted<PromisedSubrequestChannel>(
        promise.then([self = addRefToThis(), name = kj::mv(name), props = kj::mv(props),
                         limits = kj::mv(limits)]() mutable {
      return self->getEntrypointResolved(kj::mv(name), kj::mv(props), kj::mv(limits));
    }));
  } else {
    return getEntrypointResolved(kj::mv(name), kj::mv(props), kj::mv(limits));
  }
}

kj::Own<IoChannelFactory::ActorClassChannel> WorkerStubChannel::getActorClass(
    kj::Maybe<kj::String> name, Frankenvalue props, kj::Maybe<ResourceLimits> limits) {
  KJ_IF_SOME(promise, props.resolveCaps(resolveCap)) {
    return kj::refcounted<PromisedActorClassChannel>(
        promise.then([self = addRefToThis(), name = kj::mv(name), props = kj::mv(props),
                         limits = kj::mv(limits)]() mutable {
      return self->getActorClassResolved(kj::mv(name), kj::mv(props), kj::mv(limits));
    }));
  } else {
    return getActorClassResolved(kj::mv(name), kj::mv(props), kj::mv(limits));
  }
}

kj::Own<IoChannelFactory::SubrequestChannel> IoChannelFactory::subrequestChannelFromToken(
    ChannelTokenUsage usage, kj::Promise<kj::Array<byte>> token) {
  return kj::refcounted<PromisedSubrequestChannel>(token.then([this, usage](kj::Array<byte> token) {
    return subrequestChannelFromToken(usage, token.asPtr());
  }));
}

kj::Own<IoChannelFactory::ActorClassChannel> IoChannelFactory::actorClassFromToken(
    ChannelTokenUsage usage, kj::Promise<kj::Array<byte>> token) {
  return kj::refcounted<PromisedActorClassChannel>(token.then(
      [this, usage](kj::Array<byte> token) { return actorClassFromToken(usage, token.asPtr()); }));
}

kj::Own<IoChannelFactory::RpcChannel> IoChannelFactory::rpcChannelFromToken(
    ChannelTokenUsage usage, kj::Promise<kj::Array<byte>> token) {
  return kj::refcounted<PromisedRpcChannel>(token.then(
      [this, usage](kj::Array<byte> token) { return rpcChannelFromToken(usage, token.asPtr()); }));
}

kj::Promise<void> DynamicWorkerSource::ensureAllResolved() {
  kj::Vector<kj::Promise<void>> promises;

  KJ_IF_SOME(promise, env.resolveCaps(resolveCap)) {
    promises.add(kj::mv(promise));
  }

  auto resolveChannelSlot = [&](kj::Own<IoChannelFactory::SubrequestChannel>& slot) {
    KJ_SWITCH_ONEOF(slot->getResolved()) {
      KJ_CASE_ONEOF(channel, kj::Own<IoChannelFactory::TokenizableChannel>) {
        slot = channel.downcast<IoChannelFactory::SubrequestChannel>();
      }
      KJ_CASE_ONEOF(promise, kj::Promise<kj::Own<IoChannelFactory::TokenizableChannel>>) {
        promises.add(promise.then([&slot](kj::Own<IoChannelFactory::TokenizableChannel> channel) {
          slot = channel.downcast<IoChannelFactory::SubrequestChannel>();
        }));
      }
    }
  };

  KJ_IF_SOME(slot, globalOutbound) {
    resolveChannelSlot(slot);
  }

  for (auto& slot: tails) {
    resolveChannelSlot(slot);
  }
  for (auto& slot: streamingTails) {
    resolveChannelSlot(slot);
  }

  if (!promises.empty()) {
    co_await kj::joinPromisesFailFast(promises.releaseAsArray());
  }
}

kj::Promise<void> Worker::Actor::FacetManager::StartInfo::ensureAllResolved() {
  KJ_SWITCH_ONEOF(actorClass->getResolved()) {
    KJ_CASE_ONEOF(channel, kj::Own<IoChannelFactory::TokenizableChannel>) {
      actorClass = channel.downcast<IoChannelFactory::ActorClassChannel>();
    }
    KJ_CASE_ONEOF(promise, kj::Promise<kj::Own<IoChannelFactory::TokenizableChannel>>) {
      actorClass = (co_await promise).downcast<IoChannelFactory::ActorClassChannel>();
    }
  }
}

uint IoChannelCapTableEntry::getChannelNumber(Type expectedType) {
  // A type mismatch shouldn't be possible as long as attackers cannot tamper with the
  // serialization, but we do the check to catch bugs.
  KJ_REQUIRE(type == expectedType,
      "IoChannelCapTableEntry type didn't match serialized JavaScript API type.");

  return channel;
}

kj::Own<Frankenvalue::CapTableEntry> IoChannelCapTableEntry::clone() {
  return kj::heap<IoChannelCapTableEntry>(type, channel);
}

kj::Own<Frankenvalue::CapTableEntry> IoChannelCapTableEntry::threadSafeClone() const {
  return kj::heap<IoChannelCapTableEntry>(type, channel);
}

template <>
kj::Own<IoChannelFactory::SubrequestChannel> newPromisedChannel<
    IoChannelFactory::SubrequestChannel>(
    kj::Promise<kj::Own<IoChannelFactory::SubrequestChannel>> promise) {
  return kj::refcounted<PromisedSubrequestChannel>(kj::mv(promise));
}

template <>
kj::Own<IoChannelFactory::ActorClassChannel> newPromisedChannel<
    IoChannelFactory::ActorClassChannel>(
    kj::Promise<kj::Own<IoChannelFactory::ActorClassChannel>> promise) {
  return kj::refcounted<PromisedActorClassChannel>(kj::mv(promise));
}

template <>
kj::Own<IoChannelFactory::RpcChannel> newPromisedChannel<IoChannelFactory::RpcChannel>(
    kj::Promise<kj::Own<IoChannelFactory::RpcChannel>> promise) {
  return kj::refcounted<PromisedRpcChannel>(kj::mv(promise));
}

}  // namespace workerd
