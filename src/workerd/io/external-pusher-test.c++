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

}  // namespace
}  // namespace workerd
