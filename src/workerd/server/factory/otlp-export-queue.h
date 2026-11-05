// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/util/ring-buffer.h>

#include <kj/async.h>
#include <kj/timer.h>

namespace workerd::server {

using kj::uint;

// Bounded, server-wide delivery of encoded ExportTraceServiceRequests. The transport owns the
// collector connection; this queue owns batching, concurrent requests, and their timeouts.
class OtlpExportQueue final: private kj::TaskSet::ErrorHandler {
 public:
  class Transport {
   public:
    virtual ~Transport() noexcept(false) = default;
    // Completes after the collector's response body has been consumed. Dropping the promise
    // cancels the request and releases its resources.
    virtual kj::Promise<void> post(kj::Array<const kj::byte> request) = 0;
  };

  OtlpExportQueue(kj::Own<Transport> transport, kj::Timer& timer);
  ~OtlpExportQueue() noexcept(false);
  KJ_DISALLOW_COPY_AND_MOVE(OtlpExportQueue);

  void exportSpans(kj::Array<const kj::byte> request, uint spanCount);
  // Disables exports before cancelling requests, which may themselves close exported spans.
  void unlink();

 private:
  // Shared across the server's workers, with the same bounds as edgeworker's otlp-receptor.
  static constexpr size_t QUEUE_SIZE = 4096;
  static constexpr uint MAX_CONCURRENT_REQUESTS = 16;
  static constexpr size_t BATCH_SIZE_BYTES = 1024 * 1024;
  static constexpr kj::Duration REQUEST_TIMEOUT = 10 * kj::SECONDS;

  kj::Maybe<kj::Own<Transport>> transport;
  kj::Timer& timer;
  RingBuffer<kj::Array<const kj::byte>> pending;
  uint running = 0;
  kj::TaskSet tasks{*this};

  kj::Array<const kj::byte> takeBatch();
  kj::Promise<void> run();
  void taskFailed(kj::Exception&& exception) override;
};

}  // namespace workerd::server
