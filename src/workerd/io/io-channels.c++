#include "io-channels.h"

#include <workerd/io/worker-interface.h>
#include <workerd/io/worker.h>
#include <workerd/util/entropy.h>

#include <random>

namespace workerd {

IoChannelFactory::ActorRetryRequestMetadata generateActorRetryRequestMetadata(
    kj::Date createdAt, ActorRetryGateEnabled retryGateEnabled) {
  static thread_local auto generator = [] {
    uint64_t seed;
    getEntropy(kj::asBytes(seed));
    return std::mt19937_64(seed);
  }();
  std::uniform_int_distribution<uint64_t> distribution(0, UINT64_MAX);

  return IoChannelFactory::ActorRetryRequestMetadata{
    .nonce = distribution(generator),
    .createdAt = createdAt,
    .isRetry = IsActorRetry::NO,
    .retryGateEnabled = retryGateEnabled,
  };
}

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

kj::Rc<IoChannelFactory::SubrequestChannel> IoChannelFactory::subrequestChannelFromToken(
    ChannelTokenUsage usage, kj::ArrayPtr<const byte> token) {
  JSG_FAIL_REQUIRE(DOMDataCloneError, "This Worker is not able to deserialize ServiceStubs.");
}

kj::Rc<IoChannelFactory::ActorClassChannel> IoChannelFactory::actorClassFromToken(
    ChannelTokenUsage usage, kj::ArrayPtr<const byte> token) {
  JSG_FAIL_REQUIRE(
      DOMDataCloneError, "This Worker is not able to deserialize Durable Object class stubs.");
}

kj::Rc<IoChannelFactory::RpcChannel> IoChannelFactory::rpcChannelFromToken(
    ChannelTokenUsage usage, kj::ArrayPtr<const byte> token) {
  JSG_FAIL_REQUIRE(DOMDataCloneError, "This Worker is not able to deserialize RpcStubs.");
}

kj::Rc<IoChannelFactory::SubrequestChannel> IoChannelFactory::makeRestoredSubrequestChannelResolved(
    kj::Rc<SelfTokenFactory> selfTokenFactory,
    Frankenvalue restoreParams,
    kj::Rc<SubrequestChannel> inner,
    Persistent persistent) {
  KJ_UNIMPLEMENTED("This runtime doesn't support persistent ServiceStubs.");
}

kj::Rc<IoChannelFactory::RpcChannel> IoChannelFactory::makeRestoredRpcChannelResolved(
    kj::Rc<SelfTokenFactory> selfTokenFactory, Frankenvalue restoreParams, Persistent persistent) {
  KJ_UNIMPLEMENTED("This runtime doesn't support persistent RpcStubs.");
}

namespace {

template <typename ChannelType>
class PromisedTokenizableChannel: public ChannelType {
 public:
  PromisedTokenizableChannel(kj::Promise<kj::Rc<ChannelType>> promise)
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
      return readyPromise.addBranch().then(
          [self = this->addWeakToThis(), usage]() -> kj::Promise<kj::Array<byte>> {
        auto& channel = self.assertLive();
        KJ_SWITCH_ONEOF(KJ_ASSERT_NONNULL(channel.inner)->getTokenMaybeSync(usage)) {
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

  kj::OneOf<kj::Rc<IoChannelFactory::TokenizableChannel>,
      kj::Promise<kj::Rc<IoChannelFactory::TokenizableChannel>>>
  getResolved() override {
    KJ_IF_SOME(channel, inner) {
      return kj::Rc<IoChannelFactory::TokenizableChannel>(channel.addRef());
    } else {
      return readyPromise.addBranch().then([self = this->addWeakToThis()]() mutable {
        auto& channel = self.assertLive();
        return kj::Rc<IoChannelFactory::TokenizableChannel>(
            KJ_ASSERT_NONNULL(channel.inner).addRef());
      });
    }
  }

 protected:
  kj::Maybe<kj::Rc<ChannelType>> inner;
  kj::ForkedPromise<void> readyPromise;

  kj::Promise<void> waitForResolution(kj::Promise<kj::Rc<ChannelType>> promise) {
    auto resolution = co_await promise;

    KJ_SWITCH_ONEOF(resolution->getResolved()) {
      KJ_CASE_ONEOF(channel, kj::Rc<IoChannelFactory::TokenizableChannel>) {
        inner = kj::mv(channel).template downcast<ChannelType>();
        co_return;
      }
      KJ_CASE_ONEOF(deeperPromise, kj::Promise<kj::Rc<IoChannelFactory::TokenizableChannel>>) {
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
                              .then([self = addWeakToThis()]() {
        auto& channel = self.assertLive();
        auto innerRestore = KJ_ASSERT_NONNULL(channel.inner)->restore();
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
      KJ_CASE_ONEOF(channel, kj::Rc<IoChannelFactory::TokenizableChannel>) {
        return kj::implicitCast<kj::Own<Frankenvalue::CapTableEntry>>(channel.toOwn());
      }
      KJ_CASE_ONEOF(promise, kj::Promise<kj::Rc<IoChannelFactory::TokenizableChannel>>) {
        return promise
            .then([](kj::Rc<IoChannelFactory::TokenizableChannel> channel) {
          return kj::implicitCast<kj::Own<Frankenvalue::CapTableEntry>>(channel.toOwn());
        }).attach(kj::mv(cap));
      }
    }
    KJ_UNREACHABLE;
  } else {
    auto& ref = *cap;
    KJ_FAIL_ASSERT("unknown type in Frankenvalue", typeid(ref).name());
  }
}

}  // namespace

kj::Rc<IoChannelFactory::SubrequestChannel> IoChannelFactory::getSubrequestChannel(uint channel,
    kj::Maybe<Frankenvalue> props,
    kj::Maybe<VersionRequest> versionRequest,
    Persistent persistent) {
  KJ_IF_SOME(p, props) {
    KJ_IF_SOME(promise, p.resolveCaps(resolveCap)) {
      return kj::rc<PromisedSubrequestChannel>(
          promise.then([self = addRefToThis(), channel, props = kj::mv(p),
                           versionRequest = kj::mv(versionRequest), persistent]() mutable {
        return self->getSubrequestChannelResolved(
            channel, kj::mv(props), kj::mv(versionRequest), persistent);
      }));
    }
  }
  return getSubrequestChannelResolved(channel, kj::mv(props), kj::mv(versionRequest), persistent);
}

kj::Rc<IoChannelFactory::ActorClassChannel> IoChannelFactory::getActorClass(
    uint channel, kj::Maybe<Frankenvalue> props, Persistent persistent) {
  KJ_IF_SOME(p, props) {
    KJ_IF_SOME(promise, p.resolveCaps(resolveCap)) {
      return kj::rc<PromisedActorClassChannel>(
          promise.then([self = addRefToThis(), channel, props = kj::mv(p), persistent]() mutable {
        return self->getActorClassResolved(channel, kj::mv(props), persistent);
      }));
    }
  }
  return getActorClassResolved(channel, kj::mv(props), persistent);
}

kj::Rc<IoChannelFactory::SubrequestChannel> IoChannelFactory::makeRestoredSubrequestChannel(
    kj::Rc<SelfTokenFactory> selfTokenFactory,
    Frankenvalue restoreParams,
    kj::Rc<SubrequestChannel> inner,
    Persistent persistent) {
  // Note that `inner` doesn't need to be resolved since it's only used to forward requests.
  // So, the only thing we might have to wait for is `restoreParams`. Which is good as otherwise
  // this method would get a lot more complicated!

  KJ_IF_SOME(promise, restoreParams.resolveCaps(resolveCap)) {
    return kj::rc<PromisedSubrequestChannel>(promise.then(
        [self = addRefToThis(), selfTokenFactory = kj::mv(selfTokenFactory),
            restoreParams = kj::mv(restoreParams), inner = kj::mv(inner), persistent]() mutable {
      return self->makeRestoredSubrequestChannelResolved(
          kj::mv(selfTokenFactory), kj::mv(restoreParams), kj::mv(inner), persistent);
    }));
  }

  return makeRestoredSubrequestChannelResolved(
      kj::mv(selfTokenFactory), kj::mv(restoreParams), kj::mv(inner), persistent);
}

kj::Rc<IoChannelFactory::RpcChannel> IoChannelFactory::makeRestoredRpcChannel(
    kj::Rc<SelfTokenFactory> selfTokenFactory, Frankenvalue restoreParams, Persistent persistent) {
  KJ_IF_SOME(promise, restoreParams.resolveCaps(resolveCap)) {
    return kj::rc<PromisedRpcChannel>(
        promise.then([self = addRefToThis(), selfTokenFactory = kj::mv(selfTokenFactory),
                         restoreParams = kj::mv(restoreParams), persistent]() mutable {
      return self->makeRestoredRpcChannelResolved(
          kj::mv(selfTokenFactory), kj::mv(restoreParams), persistent);
    }));
  }

  return makeRestoredRpcChannelResolved(
      kj::mv(selfTokenFactory), kj::mv(restoreParams), persistent);
}

kj::Rc<IoChannelFactory::SubrequestChannel> WorkerStubChannel::getEntrypoint(
    kj::Maybe<kj::String> name, Frankenvalue props, kj::Maybe<ResourceLimits> limits) {
  KJ_IF_SOME(promise, props.resolveCaps(resolveCap)) {
    return kj::rc<PromisedSubrequestChannel>(
        promise.then([self = addRefToThis(), name = kj::mv(name), props = kj::mv(props),
                         limits = kj::mv(limits)]() mutable {
      return self->getEntrypointResolved(kj::mv(name), kj::mv(props), kj::mv(limits));
    }));
  } else {
    return getEntrypointResolved(kj::mv(name), kj::mv(props), kj::mv(limits));
  }
}

kj::Rc<IoChannelFactory::ActorClassChannel> WorkerStubChannel::getActorClass(
    kj::Maybe<kj::String> name, Frankenvalue props, kj::Maybe<ResourceLimits> limits) {
  KJ_IF_SOME(promise, props.resolveCaps(resolveCap)) {
    return kj::rc<PromisedActorClassChannel>(
        promise.then([self = addRefToThis(), name = kj::mv(name), props = kj::mv(props),
                         limits = kj::mv(limits)]() mutable {
      return self->getActorClassResolved(kj::mv(name), kj::mv(props), kj::mv(limits));
    }));
  } else {
    return getActorClassResolved(kj::mv(name), kj::mv(props), kj::mv(limits));
  }
}

kj::Rc<IoChannelFactory::SubrequestChannel> IoChannelFactory::subrequestChannelFromToken(
    ChannelTokenUsage usage, kj::Promise<kj::Array<byte>> token) {
  return kj::rc<PromisedSubrequestChannel>(
      token.then([self = addRefToThis(), usage](kj::Array<byte> token) mutable {
    return self->subrequestChannelFromToken(usage, token.asPtr());
  }));
}

kj::Rc<IoChannelFactory::ActorClassChannel> IoChannelFactory::actorClassFromToken(
    ChannelTokenUsage usage, kj::Promise<kj::Array<byte>> token) {
  return kj::rc<PromisedActorClassChannel>(
      token.then([self = addRefToThis(), usage](kj::Array<byte> token) mutable {
    return self->actorClassFromToken(usage, token.asPtr());
  }));
}

kj::Rc<IoChannelFactory::RpcChannel> IoChannelFactory::rpcChannelFromToken(
    ChannelTokenUsage usage, kj::Promise<kj::Array<byte>> token) {
  return kj::rc<PromisedRpcChannel>(
      token.then([self = addRefToThis(), usage](kj::Array<byte> token) mutable {
    return self->rpcChannelFromToken(usage, token.asPtr());
  }));
}

kj::Promise<void> DynamicWorkerSource::ensureAllResolved() {
  kj::Vector<kj::Promise<void>> promises;

  KJ_IF_SOME(promise, env.resolveCaps(resolveCap)) {
    promises.add(kj::mv(promise));
  }

  auto resolveChannelSlot = [&](auto& slot) {
    KJ_SWITCH_ONEOF(slot->getResolved()) {
      KJ_CASE_ONEOF(channel, kj::Rc<IoChannelFactory::TokenizableChannel>) {
        slot = kj::mv(channel).template downcast<IoChannelFactory::SubrequestChannel>();
      }
      KJ_CASE_ONEOF(promise, kj::Promise<kj::Rc<IoChannelFactory::TokenizableChannel>>) {
        promises.add(promise.then([&slot](kj::Rc<IoChannelFactory::TokenizableChannel> channel) {
          slot = kj::mv(channel).template downcast<IoChannelFactory::SubrequestChannel>();
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
    KJ_CASE_ONEOF(channel, kj::Rc<IoChannelFactory::TokenizableChannel>) {
      actorClass = channel.downcast<IoChannelFactory::ActorClassChannel>();
    }
    KJ_CASE_ONEOF(promise, kj::Promise<kj::Rc<IoChannelFactory::TokenizableChannel>>) {
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
kj::Rc<IoChannelFactory::SubrequestChannel> newPromisedChannel<IoChannelFactory::SubrequestChannel>(
    kj::Promise<kj::Rc<IoChannelFactory::SubrequestChannel>> promise) {
  return kj::rc<PromisedSubrequestChannel>(kj::mv(promise));
}

template <>
kj::Rc<IoChannelFactory::ActorClassChannel> newPromisedChannel<IoChannelFactory::ActorClassChannel>(
    kj::Promise<kj::Rc<IoChannelFactory::ActorClassChannel>> promise) {
  return kj::rc<PromisedActorClassChannel>(kj::mv(promise));
}

template <>
kj::Rc<IoChannelFactory::RpcChannel> newPromisedChannel<IoChannelFactory::RpcChannel>(
    kj::Promise<kj::Rc<IoChannelFactory::RpcChannel>> promise) {
  return kj::rc<PromisedRpcChannel>(kj::mv(promise));
}

}  // namespace workerd
