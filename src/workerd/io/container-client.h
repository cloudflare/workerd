#pragma once

#include <workerd/rust/container/lib.rs.h>
#include <workerd/rust/cxx-integration/cxx-bridge.h>

#include <stdlib.h>

#include <kj/async-io.h>
#include <kj/debug.h>
#include <kj/exception.h>

namespace workerd::io {
struct ContainerAsyncStream final: public kj::AsyncIoStream {
  ContainerAsyncStream(::rust::Box<rust::container::ContainerService> service)
      : service(kj::mv(service)) {}

  void shutdownWrite() override {
    service->shutdown_write();
  }

  kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override {
    // For now, return 0 bytes read (EOF) since this is primarily a write-only stream
    // In a full implementation, this would read from the container's output
    (void)buffer;
    (void)minBytes;
    (void)maxBytes;
    return size_t(0);
  }

  kj::Promise<void> write(kj::ArrayPtr<const kj::byte> buffer) override {
    if (service->write_data(buffer.as<Rust>())) {
      return kj::READY_NOW;
    } else {
      return KJ_EXCEPTION(DISCONNECTED, "Write failed: stream is disconnected");
    }
  }

  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const byte>> pieces) override {
    // Write each piece sequentially
    for (auto piece: pieces) {
      if (!service->write_data(piece.as<Rust>())) {
        return KJ_EXCEPTION(DISCONNECTED, "Write failed: stream is disconnected");
      }
    }
    return kj::READY_NOW;
  }

  kj::Promise<void> whenWriteDisconnected() override {
    if (service->is_write_disconnected()) {
      return kj::READY_NOW;
    }

    // TODO(soon): Implement this.
    // In a full implementation, this would return a promise that resolves
    // when the write side becomes disconnected. For now, we return a promise
    // that never resolves if not already disconnected.
    return kj::NEVER_DONE;
  }

  ::rust::Box<rust::container::ContainerService> service;
};
}  // namespace workerd::io
