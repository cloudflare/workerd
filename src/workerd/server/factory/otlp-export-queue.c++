// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "otlp-export-queue.h"

#include <workerd/util/sentry.h>

#include <kj/vector.h>

namespace workerd::server {

OtlpExportQueue::OtlpExportQueue(
    kj::Own<Transport> transport, kj::Timer& timer, kj::Duration linger)
    : transport(kj::mv(transport)),
      timer(timer),
      linger(linger) {}

OtlpExportQueue::~OtlpExportQueue() noexcept(false) {
  unlink();
}

void OtlpExportQueue::unlink() {
  auto transport = kj::mv(this->transport);
  this->transport = kj::none;
  pending.clear();
  pendingBytes = 0;
  tasks.clear();
  transport = kj::none;
}

void OtlpExportQueue::exportSpans(kj::Array<const kj::byte> request, uint spanCount) {
  if (transport == kj::none) return;
  if (pending.size() >= QUEUE_SIZE) {
    LOG_WARNING_PERIODICALLY("OTLP export queue is full; dropping spans", spanCount);
    return;
  }
  pendingBytes += request.size();
  pending.push_back(kj::mv(request));
  bool batch = linger == 0 * kj::SECONDS || running == 0 || pendingBytes >= BATCH_SIZE_BYTES;
  if (batch && running < MAX_CONCURRENT_REQUESTS) {
    ++running;
    tasks.add(run());
  }
}

kj::Array<const kj::byte> OtlpExportQueue::takeBatch() {
  auto first = kj::mv(pending.front());
  pending.pop_front();
  pendingBytes -= first.size();
  if (pending.empty() || first.size() >= BATCH_SIZE_BYTES) return first;

  // Concatenated ExportTraceServiceRequests join their repeated resource_spans fields.
  // Stop after reaching the threshold, exceeding it by at most one encoded request.
  kj::Vector<kj::byte> batch(first.size());
  batch.addAll(first);
  while (!pending.empty() && batch.size() < BATCH_SIZE_BYTES) {
    pendingBytes -= pending.front().size();
    batch.addAll(pending.front());
    pending.pop_front();
  }
  return batch.releaseAsArray();
}

kj::Promise<void> OtlpExportQueue::run() {
  KJ_DEFER(--running);
  while (!pending.empty()) {
    if (pendingBytes < BATCH_SIZE_BYTES && linger > 0 * kj::SECONDS) {
      co_await timer.afterDelay(linger);
      if (pending.empty()) break;
    }
    auto request = takeBatch();
    // Use a continuation to avoid Windows coroutine catch miscompilation. evalNow also
    // covers synchronous failures during collector request setup.
    co_await kj::evalNow([&]() {
      return timer.timeoutAfter(
          REQUEST_TIMEOUT, KJ_ASSERT_NONNULL(transport)->post(kj::mv(request)));
    }).catch_([this](kj::Exception&& exception) {
      // Failed batches are not retried. Keep draining so a collector can recover.
      taskFailed(kj::mv(exception));
    });
  }
}

void OtlpExportQueue::taskFailed(kj::Exception&& exception) {
  KJ_LOG(ERROR, "error exporting spans to the OTLP collector", exception);
}

}  // namespace workerd::server
