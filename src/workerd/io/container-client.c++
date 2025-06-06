// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "container-client.h"

#include <kj/array.h>
#include <kj/debug.h>

namespace workerd::io {

ContainerStreamSharedState::ContainerStreamSharedState() {
  callback = [self = addRefToThis()](
                 ::rust::Slice<const uint8_t> message) mutable { self->enqueueMessage(message); };
}

void ContainerStreamSharedState::enqueueMessage(::rust::Slice<const uint8_t> message) const {
  auto lockedQueue = messageQueue.lockExclusive();
  for (auto& byte: message) {
    lockedQueue->push(byte);
  }

  auto lockedWaiter = readWaiter.lockExclusive();
  KJ_IF_SOME(fulfiller, *lockedWaiter) {
    fulfiller->fulfill();
    *lockedWaiter = kj::none;
  }
}

kj::Maybe<size_t> ContainerStreamSharedState::tryRead(
    void* buffer, size_t minBytes, size_t maxBytes) const {
  auto lockedQueue = messageQueue.lockExclusive();
  if (lockedQueue->empty()) {
    return kj::none;
  }

  size_t min = kj::min(lockedQueue->size(), maxBytes);
  KJ_REQUIRE(min > 0, "Should never happen");
  memcpy(buffer, &lockedQueue->front(), min);
  for (auto i = 0; i < min; ++i) {
    lockedQueue->pop();
  }
  return min;
}

kj::Promise<void> ContainerStreamSharedState::waitForMessage() const {
  auto lockedWaiter = readWaiter.lockExclusive();
  KJ_REQUIRE(*lockedWaiter == kj::none, "Only one reader can wait at a time");

  auto paf = kj::newPromiseAndCrossThreadFulfiller<void>();
  *lockedWaiter = kj::mv(paf.fulfiller);
  return kj::mv(paf.promise);
}

void ContainerAsyncStream::shutdownWrite() {
  service->shutdown_write();
}

kj::Promise<size_t> ContainerAsyncStream::tryRead(void* buffer, size_t minBytes, size_t maxBytes) {
  KJ_IF_SOME(consumed, sharedState->tryRead(buffer, minBytes, maxBytes)) {
    co_return consumed;
  }

  if (minBytes == 0) {
    co_return minBytes;
  }

  co_await sharedState->waitForMessage();
  auto n = co_await tryRead(buffer, minBytes, maxBytes);
  co_return n;
}

kj::Promise<void> ContainerAsyncStream::write(kj::ArrayPtr<const kj::byte> buffer) {
  if (!service->write_data(buffer.as<Rust>())) {
    return KJ_EXCEPTION(DISCONNECTED, "Write failed: stream is disconnected");
  }
  return kj::READY_NOW;
}

kj::Promise<void> ContainerAsyncStream::write(
    kj::ArrayPtr<const kj::ArrayPtr<const kj::byte>> pieces) {
  for (auto piece: pieces) {
    if (!service->write_data(piece.as<Rust>())) {
      return KJ_EXCEPTION(DISCONNECTED, "Write failed: stream is disconnected");
    }
  }
  return kj::READY_NOW;
}

kj::Promise<void> ContainerAsyncStream::whenWriteDisconnected() {
  // TODO(now): this is wrong, the returned promise should fulfill when the write end disconnects.
  // as written this will only return a fulfilled promise if the write end already disconnected.
  if (service->is_write_disconnected()) {
    return kj::READY_NOW;
  }
  return kj::NEVER_DONE;
}

kj::Own<ContainerAsyncStream> createContainerRpcStream(
    kj::StringPtr address, kj::StringPtr containerName) {
  auto sharedState = kj::arc<ContainerStreamSharedState>();

  auto service = rust::container::new_service(
      address.cStr(), containerName.cStr(), sharedState->messageCallback());
  return kj::heap<ContainerAsyncStream>(kj::mv(service), kj::mv(sharedState));
}

}  // namespace workerd::io
