#include "io-channels.h"

#include <workerd/io/worker-interface.h>

namespace workerd {

kj::Promise<kj::Array<byte>> IoChannelFactory::SubrequestChannel::getToken(
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

kj::Promise<kj::Array<byte>> IoChannelFactory::ActorClassChannel::getToken(
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

namespace {

class PromisedSubrequestChannel final: public IoChannelFactory::SubrequestChannel {
 public:
  PromisedSubrequestChannel(kj::Promise<kj::Own<SubrequestChannel>> promise)
      : readyPromise(waitForResolution(kj::mv(promise)).fork()) {}

  kj::Own<WorkerInterface> startRequest(IoChannelFactory::SubrequestMetadata metadata) override {
    KJ_IF_SOME(channel, inner) {
      return channel->startRequest(kj::mv(metadata));
    } else {
      return newPromisedWorkerInterface(
          readyPromise.addBranch().then([this, metadata = kj::mv(metadata)]() mutable {
        return KJ_ASSERT_NONNULL(inner)->startRequest(kj::mv(metadata));
      }));
    }
  }

  void requireAllowsTransfer() override {
    // PromisedSubrequestChannel is used for channels initialized from a promised channel token.
    // A SubrequestChannel created from a channel token should always support transfer, via channel
    // tokens.
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

  kj::OneOf<kj::Own<SubrequestChannel>, kj::Promise<kj::Own<SubrequestChannel>>> getResolved()
      override {
    KJ_IF_SOME(channel, inner) {
      return kj::addRef(*channel);
    } else {
      return readyPromise.addBranch().then(
          [this]() mutable { return kj::addRef(*KJ_ASSERT_NONNULL(inner)); });
    }
  }

 private:
  kj::ForkedPromise<void> readyPromise;
  kj::Maybe<kj::Own<SubrequestChannel>> inner;

  kj::Promise<void> waitForResolution(kj::Promise<kj::Own<SubrequestChannel>> promise) {
    for (;;) {
      auto resolution = co_await promise;
      KJ_SWITCH_ONEOF(resolution->getResolved()) {
        KJ_CASE_ONEOF(channel, kj::Own<SubrequestChannel>) {
          inner = kj::mv(channel);
          co_return;
        }
        KJ_CASE_ONEOF(deeperPromise, kj::Promise<kj::Own<SubrequestChannel>>) {
          // Promise resolved to another promise, wait for it too.
          promise = kj::mv(deeperPromise);
        }
      }
    }
  }
};

class PromisedActorClassChannel final: public IoChannelFactory::ActorClassChannel {
 public:
  PromisedActorClassChannel(kj::Promise<kj::Own<ActorClassChannel>> promise)
      : readyPromise(waitForResolution(kj::mv(promise)).fork()) {}

  void requireAllowsTransfer() override {
    // PromisedActorClassChannel is used for channels initialized from a promised channel token.
    // A ActorClassChannel created from a channel token should always support transfer, via channel
    // tokens.
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

  kj::OneOf<kj::Own<ActorClassChannel>, kj::Promise<kj::Own<ActorClassChannel>>> getResolved()
      override {
    KJ_IF_SOME(channel, inner) {
      return kj::addRef(*channel);
    } else {
      return readyPromise.addBranch().then(
          [this]() mutable { return kj::addRef(*KJ_ASSERT_NONNULL(inner)); });
    }
  }

 private:
  kj::ForkedPromise<void> readyPromise;
  kj::Maybe<kj::Own<ActorClassChannel>> inner;

  kj::Promise<void> waitForResolution(kj::Promise<kj::Own<ActorClassChannel>> promise) {
    for (;;) {
      auto resolution = co_await promise;
      KJ_SWITCH_ONEOF(resolution->getResolved()) {
        KJ_CASE_ONEOF(channel, kj::Own<ActorClassChannel>) {
          inner = kj::mv(channel);
          co_return;
        }
        KJ_CASE_ONEOF(deeperPromise, kj::Promise<kj::Own<ActorClassChannel>>) {
          promise = kj::mv(deeperPromise);
        }
      }
    }
  }
};

}  // namespace

kj::Own<IoChannelFactory::SubrequestChannel> IoChannelFactory::subrequestChannelFromToken(
    ChannelTokenUsage usage, kj::Promise<kj::Array<byte>> token) {
  return kj::refcounted<PromisedSubrequestChannel>(token.then([this, usage](kj::Array<byte> token) {
    return subrequestChannelFromToken(usage, token.asPtr());
  }));
}

kj::Own<IoChannelFactory::ActorClassChannel> IoChannelFactory::actorClassFromToken(
    ChannelTokenUsage usage, kj::Promise<kj::Array<byte>> token) {
  // We shouldn't get here unless someone (Kenton) screwed up the build order.
  KJ_FAIL_ASSERT("Async channel token deserialization unimplemented.");
}

void IoChannelFactory::ActorChannel::requireAllowsTransfer() {
  JSG_FAIL_REQUIRE(DOMDataCloneError,
      "Durable Object stubs cannot (yet) be transferred between Workers. This will change in "
      "a future version.");
}

kj::OneOf<kj::Array<byte>, kj::Promise<kj::Array<byte>>> IoChannelFactory::ActorChannel::
    getTokenMaybeSync(ChannelTokenUsage usage) {
  JSG_FAIL_REQUIRE(DOMDataCloneError,
      "Durable Object stubs cannot (yet) be transferred between Workers. This will change in "
      "a future version.");
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

}  // namespace workerd
