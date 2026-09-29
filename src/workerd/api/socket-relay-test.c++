// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "socket-relay.h"

#include <kj/test.h>

namespace workerd::api {
namespace {

// One end of a relay: the relay holds `relayed`, and the test plays the far side through `far`.
struct Link {
  kj::Own<kj::AsyncIoStream> far;
  kj::Own<kj::AsyncIoStream> relayed;
};

Link newLink() {
  auto pipe = kj::newTwoWayPipe();
  return Link{.far = kj::mv(pipe.ends[0]), .relayed = kj::mv(pipe.ends[1])};
}

kj::Promise<void> writeString(kj::AsyncOutputStream& out, kj::StringPtr text) {
  return out.write(text.asBytes());
}

kj::String readString(kj::AsyncInputStream& in, size_t size, kj::WaitScope& waitScope) {
  auto buffer = kj::heapArray<kj::byte>(size);
  in.read(buffer).wait(waitScope);
  return kj::heapString(buffer.asChars());
}

void expectEof(kj::AsyncInputStream& in, kj::WaitScope& waitScope) {
  char c;
  KJ_EXPECT(in.tryRead(&c, 1, 1).wait(waitScope) == 0);
}

// Passes reads on a few event-loop turns after they complete, the way a stack of stream adapters
// between a transport and its reader does.
class DelayedReads final: public kj::AsyncIoStream {
 public:
  explicit DelayedReads(kj::Own<kj::AsyncIoStream> inner): inner(kj::mv(inner)) {}

  kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override {
    size_t amount = co_await inner->tryRead(buffer, minBytes, maxBytes);
    for (auto i = 0; i < 3; i++) {
      co_await kj::yield();
    }
    co_return amount;
  }
  kj::Promise<void> write(kj::ArrayPtr<const kj::byte> buffer) override {
    return inner->write(buffer);
  }
  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const kj::byte>> pieces) override {
    return inner->write(pieces);
  }
  kj::Promise<void> whenWriteDisconnected() override {
    return inner->whenWriteDisconnected();
  }
  void shutdownWrite() override {
    inner->shutdownWrite();
  }

 private:
  kj::Own<kj::AsyncIoStream> inner;
};

// Fails its reads when the test says so, and otherwise never completes them.
class FailingReads final: public kj::AsyncIoStream {
 public:
  explicit FailingReads(kj::Own<kj::AsyncIoStream> inner): inner(kj::mv(inner)) {}

  void fail(kj::Exception e) {
    failure->reject(kj::mv(e));
  }

  kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override {
    return failed.addBranch().then([]() -> size_t { KJ_UNREACHABLE; });
  }
  kj::Promise<void> write(kj::ArrayPtr<const kj::byte> buffer) override {
    return inner->write(buffer);
  }
  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const kj::byte>> pieces) override {
    return inner->write(pieces);
  }
  kj::Promise<void> whenWriteDisconnected() override {
    return inner->whenWriteDisconnected();
  }
  void shutdownWrite() override {
    inner->shutdownWrite();
  }

 private:
  kj::Own<kj::AsyncIoStream> inner;
  kj::PromiseFulfillerPair<void> paf = kj::newPromiseAndFulfiller<void>();
  kj::Own<kj::PromiseFulfiller<void>> failure = kj::mv(paf.fulfiller);
  kj::ForkedPromise<void> failed = paf.promise.fork();
};

// What the test observes of an upgrade request the relay was handed.
struct UpgradeRequest {
  kj::PromiseFulfillerPair<void> asked = kj::newPromiseAndFulfiller<void>();
  bool answered = false;
  kj::Maybe<kj::Exception> failure;
};

class FakeInboundTlsUpgrade final: public InboundTlsUpgrade {
 public:
  explicit FakeInboundTlsUpgrade(UpgradeRequest& request): request(request) {}

  kj::Promise<void> whenRequested() override {
    return kj::mv(request.asked.promise);
  }
  void answer(kj::Maybe<kj::Exception> failure) override {
    KJ_EXPECT(!request.answered);
    request.answered = true;
    request.failure = kj::mv(failure);
  }

 private:
  UpgradeRequest& request;
};

// What the test observes of, and controls about, the upgrade the relay makes on the other end.
struct Upgrade {
  uint32_t calls = 0;
  kj::Maybe<kj::Own<kj::PromiseFulfiller<void>>> fulfiller;

  kj::Function<kj::Promise<void>()> starter() {
    return [this]() {
      ++calls;
      auto paf = kj::newPromiseAndFulfiller<void>();
      fulfiller = kj::mv(paf.fulfiller);
      return kj::mv(paf.promise);
    };
  }
};

KJ_TEST("relayStreams() relays both directions, each ending independently") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);
  auto a = newLink();
  auto b = newLink();

  auto relay =
      relayStreams(RelayEnd{.stream = kj::mv(a.relayed)}, RelayEnd{.stream = kj::mv(b.relayed)})
          .eagerlyEvaluate(nullptr);

  auto written = writeString(*a.far, "ping");
  KJ_EXPECT(readString(*b.far, 4, waitScope) == "ping");
  written.wait(waitScope);

  // Ending one direction leaves the other one open.
  a.far->shutdownWrite();
  expectEof(*b.far, waitScope);
  written = writeString(*b.far, "pong");
  KJ_EXPECT(readString(*a.far, 4, waitScope) == "pong");
  written.wait(waitScope);
  KJ_EXPECT(!relay.poll(waitScope));

  b.far->shutdownWrite();
  expectEof(*a.far, waitScope);
  relay.wait(waitScope);
}

KJ_TEST("relayStreams() fails when either direction fails") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);
  auto a = newLink();
  auto b = newLink();

  auto relay =
      relayStreams(RelayEnd{.stream = kj::mv(a.relayed)}, RelayEnd{.stream = kj::mv(b.relayed)})
          .eagerlyEvaluate(nullptr);

  // Once one far side has gone away, the next write to it fails.
  b.far = nullptr;
  auto written = writeString(*a.far, "data");
  KJ_EXPECT_THROW(DISCONNECTED, relay.wait(waitScope));
  written.wait(waitScope);
}

KJ_TEST("relayStreams() upgrades the other end between the bytes sent before and after asking") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);
  auto a = newLink();
  auto b = newLink();
  UpgradeRequest request;
  Upgrade upgrade;

  auto relay = relayStreams(RelayEnd{.stream = kj::heap<DelayedReads>(kj::mv(a.relayed)),
                              .inboundUpgrade = kj::heap<FakeInboundTlsUpgrade>(request)},
      RelayEnd{.stream = kj::mv(b.relayed), .startTls = upgrade.starter()})
                   .eagerlyEvaluate(nullptr);

  // The far side asks for the upgrade as soon as the bytes before it have left its transport, the
  // way a Socket does after flushing its writable. Those bytes are still on their way to the other
  // end, and the other end is not reading yet.
  auto asked = writeString(*a.far, "STARTTLS").then([&]() { request.asked.fulfiller->fulfill(); });
  asked.wait(waitScope);
  KJ_EXPECT(!relay.poll(waitScope));
  KJ_EXPECT(upgrade.calls == 0);

  // The upgrade waits for those bytes to be written.
  KJ_EXPECT(readString(*b.far, 8, waitScope) == "STARTTLS");
  KJ_EXPECT(!relay.poll(waitScope));
  KJ_EXPECT(upgrade.calls == 1);
  KJ_EXPECT(!request.answered);

  // Bytes sent after asking are held back until the upgrade is done.
  auto sentAfter = writeString(*a.far, "EHLO");
  sentAfter.wait(waitScope);
  kj::byte buffer[4];
  auto readAfter = b.far->read(buffer);
  KJ_EXPECT(!readAfter.poll(waitScope));

  KJ_ASSERT_NONNULL(upgrade.fulfiller)->fulfill();
  readAfter.wait(waitScope);
  KJ_EXPECT(kj::heapString(kj::arrayPtr(buffer).asChars()) == "EHLO");
  KJ_EXPECT(request.answered);
  KJ_EXPECT(request.failure == kj::none);

  // The opposite direction carries on through the upgrade.
  auto reply = writeString(*b.far, "250");
  KJ_EXPECT(readString(*a.far, 3, waitScope) == "250");
  reply.wait(waitScope);

  a.far->shutdownWrite();
  b.far->shutdownWrite();
  expectEof(*b.far, waitScope);
  expectEof(*a.far, waitScope);
  relay.wait(waitScope);
}

KJ_TEST("relayStreams() fails, and fails the request, when the other end cannot be upgraded") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);
  auto a = newLink();
  auto b = newLink();
  UpgradeRequest request;

  auto relay = relayStreams(RelayEnd{.stream = kj::mv(a.relayed)},
      RelayEnd{
        .stream = kj::mv(b.relayed), .inboundUpgrade = kj::heap<FakeInboundTlsUpgrade>(request)})
                   .eagerlyEvaluate(nullptr);

  // The far side of `b` asks, which upgrades `a`. That end cannot be upgraded here, so the request
  // fails, and so does the relay.
  request.asked.fulfiller->fulfill();
  KJ_EXPECT_THROW_MESSAGE("does not support startTls()", relay.wait(waitScope));
  KJ_EXPECT(request.answered);
  KJ_EXPECT(
      KJ_ASSERT_NONNULL(request.failure).getDescription().contains("does not support startTls()"));
}

KJ_TEST("relayStreams() fails, and fails the request, when the upgrade fails") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);
  auto a = newLink();
  auto b = newLink();
  UpgradeRequest request;
  Upgrade upgrade;

  auto relay = relayStreams(RelayEnd{.stream = kj::mv(a.relayed),
                              .inboundUpgrade = kj::heap<FakeInboundTlsUpgrade>(request)},
      RelayEnd{.stream = kj::mv(b.relayed), .startTls = upgrade.starter()})
                   .eagerlyEvaluate(nullptr);

  request.asked.fulfiller->fulfill();
  KJ_EXPECT(!relay.poll(waitScope));
  KJ_ASSERT(upgrade.calls == 1);
  KJ_ASSERT_NONNULL(upgrade.fulfiller)->reject(KJ_EXCEPTION(FAILED, "handshake failed"));

  KJ_EXPECT_THROW_MESSAGE("handshake failed", relay.wait(waitScope));
  KJ_EXPECT(request.answered);
  KJ_EXPECT(KJ_ASSERT_NONNULL(request.failure).getDescription() == "handshake failed");
}

KJ_TEST("relayStreams() answers an upgrade request with its own failure if it fails first") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);
  auto a = newLink();
  auto b = newLink();
  UpgradeRequest request;
  Upgrade upgrade;
  auto failingB = kj::heap<FailingReads>(kj::mv(b.relayed));
  auto& failing = *failingB;

  auto relay = relayStreams(RelayEnd{.stream = kj::mv(a.relayed),
                              .inboundUpgrade = kj::heap<FakeInboundTlsUpgrade>(request)},
      RelayEnd{.stream = kj::mv(failingB), .startTls = upgrade.starter()})
                   .eagerlyEvaluate(nullptr);

  request.asked.fulfiller->fulfill();
  KJ_EXPECT(!relay.poll(waitScope));
  KJ_ASSERT(upgrade.calls == 1);

  // The end being upgraded fails before its upgrade does, as a transport whose handshake fails
  // may report on its read side first.
  failing.fail(KJ_EXCEPTION(FAILED, "handshake failed"));
  KJ_EXPECT_THROW_MESSAGE("handshake failed", relay.wait(waitScope));
  KJ_EXPECT(request.answered);
  KJ_EXPECT(KJ_ASSERT_NONNULL(request.failure).getDescription() == "handshake failed");
}

KJ_TEST("relayStreams() carries on when the far side goes away without asking to upgrade") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);
  auto a = newLink();
  auto b = newLink();
  UpgradeRequest request;
  Upgrade upgrade;

  auto relay = relayStreams(RelayEnd{.stream = kj::mv(a.relayed),
                              .inboundUpgrade = kj::heap<FakeInboundTlsUpgrade>(request)},
      RelayEnd{.stream = kj::mv(b.relayed), .startTls = upgrade.starter()})
                   .eagerlyEvaluate(nullptr);

  request.asked.fulfiller->reject(KJ_EXCEPTION(DISCONNECTED, "peer went away"));
  KJ_EXPECT(!relay.poll(waitScope));

  auto written = writeString(*a.far, "data");
  KJ_EXPECT(readString(*b.far, 4, waitScope) == "data");
  written.wait(waitScope);

  a.far->shutdownWrite();
  b.far->shutdownWrite();
  expectEof(*b.far, waitScope);
  expectEof(*a.far, waitScope);
  relay.wait(waitScope);
  KJ_EXPECT(upgrade.calls == 0);
  KJ_EXPECT(!request.answered);
}

}  // namespace
}  // namespace workerd::api
