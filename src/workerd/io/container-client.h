// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/rust/container/lib.rs.h>
#include <workerd/rust/cxx-integration/cxx-bridge.h>

#include <stdlib.h>

#include <kj/async-io.h>
#include <kj/debug.h>
#include <kj/exception.h>
#include <kj/mutex.h>

#include <queue>

namespace workerd::io {

class ContainerStreamSharedState;

// ContainerAsyncStream provides bidirectional communication with a container.
//
// This stream implements both reading and writing:
// - Writing sends data to the container via the Rust service
// - Reading receives messages from the container via MessageCallback queuing
//
// The stream uses shared state to coordinate between the MessageCallback
// (which receives messages from the container asynchronously) and the
// tryRead() method (which provides those messages to the C++ side).
struct ContainerAsyncStream final: public kj::AsyncIoStream {
  ContainerAsyncStream(::rust::Box<rust::container::ContainerService> service,
      kj::Rc<ContainerStreamSharedState> sharedState)
      : service(kj::mv(service)),
        sharedState(kj::mv(sharedState)) {}

  void shutdownWrite() override;
  kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override;
  kj::Promise<void> write(kj::ArrayPtr<const kj::byte> buffer) override;
  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const kj::byte>> pieces) override;
  kj::Promise<void> whenWriteDisconnected() override;

 private:
  ::rust::Box<rust::container::ContainerService> service;
  kj::Rc<ContainerStreamSharedState> sharedState;
};

// Shared state between MessageCallback and ContainerAsyncStream
class ContainerStreamSharedState: public kj::Refcounted {
 public:
  ContainerStreamSharedState();

  void enqueueMessage(::rust::Slice<const uint8_t> message);
  kj::Maybe<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes);
  kj::Promise<void> waitForMessage();

 private:
  kj::MutexGuarded<std::queue<kj::byte>> messageQueue;
  kj::MutexGuarded<kj::Maybe<kj::Own<kj::PromiseFulfiller<void>>>> readWaiter;
};

kj::Own<ContainerAsyncStream> createContainerRpcStream(
    kj::StringPtr address, kj::StringPtr containerName);

}  // namespace workerd::io
