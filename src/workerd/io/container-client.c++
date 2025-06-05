// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "container-client.h"

#include <kj/array.h>
#include <kj/debug.h>

namespace workerd::io {

ContainerStreamSharedState::ContainerStreamSharedState() {}

void ContainerStreamSharedState::enqueueMessage(::rust::Slice<const uint8_t> message) {
  auto messageArray = kj::heapArray<kj::byte>(message.size());
  memcpy(messageArray.begin(), message.data(), message.size());

  auto lockedQueue = messageQueue.lockExclusive();
  lockedQueue->push(kj::mv(messageArray));

  auto lockedWaiter = readWaiter.lockExclusive();
  KJ_IF_SOME(fulfiller, *lockedWaiter) {
    fulfiller->fulfill();
    *lockedWaiter = kj::none;
  }
}

kj::Maybe<kj::Array<kj::byte>> ContainerStreamSharedState::dequeueMessage() {
  auto lockedQueue = messageQueue.lockExclusive();
  if (lockedQueue->empty()) {
    return kj::none;
  }

  auto message = kj::mv(lockedQueue->front());
  lockedQueue->pop();
  return kj::mv(message);
}

kj::Promise<void> ContainerStreamSharedState::waitForMessage() {
  auto lockedWaiter = readWaiter.lockExclusive();
  KJ_REQUIRE(*lockedWaiter == kj::none, "Only one reader can wait at a time");

  auto paf = kj::newPromiseAndFulfiller<void>();
  *lockedWaiter = kj::mv(paf.fulfiller);
  return kj::mv(paf.promise);
}

kj::Promise<size_t> ContainerAsyncStream::tryRead(void* buffer, size_t minBytes, size_t maxBytes) {
  KJ_IF_SOME(message, sharedState->dequeueMessage()) {
    size_t bytesToCopy = kj::min(message.size(), maxBytes);
    memcpy(buffer, message.begin(), bytesToCopy);
    return bytesToCopy;
  }

  if (minBytes == 0) {
    return minBytes;
  }

  return sharedState->waitForMessage().then(
      [this, buffer, minBytes, maxBytes]() -> kj::Promise<size_t> {
    return tryRead(buffer, minBytes, maxBytes);
  });
}

kj::Own<ContainerAsyncStream> createContainerRpcStream(
    kj::StringPtr address, kj::StringPtr containerName) {
  auto sharedState = kj::heap<ContainerStreamSharedState>();
  ContainerStreamSharedState* ptr = sharedState.get();

  rust::container::MessageCallback callback = [ptr](::rust::Slice<const uint8_t> message) {
    ptr->enqueueMessage(message);
  };

  auto service = rust::container::new_service(address.cStr(), containerName.cStr(), callback);
  return kj::heap<ContainerAsyncStream>(kj::mv(service), kj::mv(sharedState));
}

}  // namespace workerd::io
