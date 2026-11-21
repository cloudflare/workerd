// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "external-pusher.h"

#include <kj/test.h>

namespace workerd {
namespace {

using ExternalPusher = rpc::JsValue::ExternalPusher;

kj::Promise<kj::Array<byte>> tokenPromise(kj::StringPtr token) {
  return kj::heapArray(token.asBytes());
}

kj::String unwrap(ExternalPusherImpl& pusher,
    ExternalPusher::DelayedChannelToken::Client cap,
    kj::WaitScope& waitScope) {
  return kj::heapString(pusher.unwrapDelayedChannelToken(kj::mv(cap)).wait(waitScope).asChars());
}

KJ_TEST("delayed channel token unwraps at the pusher it was pushed to") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);
  capnp::ByteStreamFactory byteStreamFactory;
  auto pusher = kj::refcounted<ExternalPusherImpl>(byteStreamFactory);

  auto cap = sendDelayedChannelToken(kj::addRef(*pusher), tokenPromise("the-token"));
  KJ_EXPECT(unwrap(*pusher, kj::mv(cap), waitScope) == "the-token");
}

KJ_TEST("delayed channel token unwraps at a pusher it was not pushed to") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);
  capnp::ByteStreamFactory byteStreamFactory;
  auto pushedTo = kj::refcounted<ExternalPusherImpl>(byteStreamFactory);
  auto unwrappedBy = kj::refcounted<ExternalPusherImpl>(byteStreamFactory);

  // This used to fail with "pushed external is not a DelayedChannelToken".
  auto cap = sendDelayedChannelToken(kj::addRef(*pushedTo), tokenPromise("the-token"));
  KJ_EXPECT(unwrap(*unwrappedBy, cap, waitScope) == "the-token");

  // Unwrapping does not consume the token, so the pusher that holds it can still unwrap it.
  KJ_EXPECT(unwrap(*pushedTo, kj::mv(cap), waitScope) == "the-token");
}

KJ_TEST("delayed channel token pushes once the token resolves") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);
  capnp::ByteStreamFactory byteStreamFactory;
  auto pusher = kj::refcounted<ExternalPusherImpl>(byteStreamFactory);

  auto paf = kj::newPromiseAndFulfiller<kj::Array<byte>>();
  auto unwrapped = pusher->unwrapDelayedChannelToken(
      sendDelayedChannelToken(kj::addRef(*pusher), kj::mv(paf.promise)));
  KJ_EXPECT(!unwrapped.poll(waitScope));

  paf.fulfiller->fulfill(kj::heapArray("later"_kj.asBytes()));
  KJ_EXPECT(kj::heapString(unwrapped.wait(waitScope).asChars()) == "later");
}

KJ_TEST("delayed channel token reports the failure to produce the token") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);
  capnp::ByteStreamFactory byteStreamFactory;
  auto pusher = kj::refcounted<ExternalPusherImpl>(byteStreamFactory);

  auto cap = sendDelayedChannelToken(kj::addRef(*pusher),
      kj::Promise<kj::Array<byte>>(KJ_EXCEPTION(FAILED, "token exchange failed")));
  KJ_EXPECT_THROW_MESSAGE(
      "token exchange failed", pusher->unwrapDelayedChannelToken(kj::mv(cap)).wait(waitScope));
}

class DiscardingOutputStream final: public kj::AsyncOutputStream {
 public:
  kj::Promise<void> write(kj::ArrayPtr<const byte> buffer) override {
    return kj::READY_NOW;
  }
  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const byte>> pieces) override {
    return kj::READY_NOW;
  }
  kj::Promise<void> whenWriteDisconnected() override {
    return kj::NEVER_DONE;
  }
};

// The pipe's first read or pump is served by the ByteStream's probe for a shorter path, which
// finishes in a later turn. The writer goes away either while the probe still serves it, which
// cancels it, or after the probe has finished, which leaves the pipe at EOF.
KJ_TEST("a pushed byte stream whose writer goes away before end() reports a premature disconnect") {
  for (bool probeFinished: {false, true}) {
    for (bool pump: {false, true}) {
      kj::EventLoop loop;
      kj::WaitScope waitScope(loop);
      capnp::ByteStreamFactory byteStreamFactory;
      auto pusher = kj::refcounted<ExternalPusherImpl>(byteStreamFactory);
      ExternalPusher::Client client = kj::addRef(*pusher);

      kj::Maybe<capnp::Response<ExternalPusher::PushByteStreamResults>> pushed =
          client.pushByteStreamRequest().send().wait(waitScope);
      auto input = pusher->unwrapStream(KJ_ASSERT_NONNULL(pushed).getSource());
      // Resolves the unwrapped stream, so that the read below reaches the pipe at once.
      waitScope.poll();

      kj::FixedArray<byte, 16> buffer;
      DiscardingOutputStream output;
      kj::Promise<uint64_t> consumed = pump
          ? input->pumpTo(output, kj::maxValue)
          : input->tryRead(buffer.begin(), 1, buffer.size()).then([](size_t n) -> uint64_t {
        return n;
      });
      if (probeFinished) {
        waitScope.poll();
      }

      // Releases the only reference to the ByteStream, as a writer that aborts does.
      pushed = kj::none;

      KJ_EXPECT_THROW_MESSAGE("ReadableStream received over RPC disconnected prematurely.",
          consumed.wait(waitScope), probeFinished, pump);
    }
  }
}

class DisconnectedOutputStream final: public kj::AsyncOutputStream {
 public:
  kj::Promise<void> write(kj::ArrayPtr<const byte> buffer) override {
    return KJ_EXCEPTION(DISCONNECTED, "the output disconnected");
  }
  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const byte>> pieces) override {
    return KJ_EXCEPTION(DISCONNECTED, "the output disconnected");
  }
  kj::Promise<void> whenWriteDisconnected() override {
    return kj::NEVER_DONE;
  }
};

// The failure of a pump's output, such as the client of a response that streams the body going
// away, is the output's, while the writer is still there to send more.
KJ_TEST("a pushed byte stream pumped to an output that disconnects fails as the output does") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);
  capnp::ByteStreamFactory byteStreamFactory;
  auto pusher = kj::refcounted<ExternalPusherImpl>(byteStreamFactory);
  ExternalPusher::Client client = kj::addRef(*pusher);

  auto pushed = client.pushByteStreamRequest().send().wait(waitScope);
  auto input = pusher->unwrapStream(pushed.getSource());
  auto writer = byteStreamFactory.capnpToKj(pushed.getSink());
  // Resolves the unwrapped stream, so that the pump below reaches the pipe at once.
  waitScope.poll();

  DisconnectedOutputStream output;
  auto consumed = input->pumpTo(output, kj::maxValue);
  // Kept until the pump fails. The writer learns of the failure only later, if at all.
  auto written = writer->write("data"_kj.asBytes());

  auto exception =
      KJ_ASSERT_NONNULL(kj::runCatchingExceptions([&]() { consumed.wait(waitScope); }));
  KJ_EXPECT(exception.getType() == kj::Exception::Type::DISCONNECTED, exception);
  KJ_EXPECT(exception.getDescription() == "the output disconnected"_kj, exception);
}

}  // namespace
}  // namespace workerd
