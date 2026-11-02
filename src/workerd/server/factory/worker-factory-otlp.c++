// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "worker-factory-impl.h"

#include <workerd/util/ring-buffer.h>
#include <workerd/util/sentry.h>

namespace workerd::server {

// Submitters may outlive the services, so exports run in their own task set rather than a
// worker's. Unlinking disables later exports and releases the collector's service graph.
class OtlpExporter::Impl final: public OtlpSpanExporter, private kj::TaskSet::ErrorHandler {
 public:
  Impl(::rust::Box<SubrequestChannel> collector,
      const kj::HttpHeaderTable& headerTable,
      kj::Timer& timer)
      : collector(kj::mv(collector)),
        headerTable(headerTable),
        timer(timer) {}

  void unlink() {
    // Cancelling requests can close spans. Disable exports before those cancellations run.
    auto collector = kj::mv(this->collector);
    this->collector = kj::none;
    pending.clear();
    tasks.clear();
    collector = kj::none;
  }

  void exportSpans(kj::Array<const kj::byte> request, uint spanCount) override {
    if (collector == kj::none) return;
    if (pending.size() >= QUEUE_SIZE) {
      LOG_WARNING_PERIODICALLY("OTLP export queue is full; dropping spans", spanCount);
      return;
    }
    pending.push_back(kj::mv(request));
    if (running < MAX_CONCURRENT_REQUESTS) {
      ++running;
      tasks.add(run());
    }
  }

 private:
  // Shared across the server's workers, with the same bounds as edgeworker's otlp-receptor.
  static constexpr size_t QUEUE_SIZE = 4096;
  static constexpr uint MAX_CONCURRENT_REQUESTS = 16;
  static constexpr size_t BATCH_SIZE_BYTES = 1024 * 1024;
  static constexpr kj::Duration REQUEST_TIMEOUT = 10 * kj::SECONDS;

  kj::Maybe<::rust::Box<SubrequestChannel>> collector;
  const kj::HttpHeaderTable& headerTable;
  kj::Timer& timer;
  RingBuffer<kj::Array<const kj::byte>> pending;
  uint running = 0;
  kj::TaskSet tasks{*this};

  kj::Array<const kj::byte> takeBatch() {
    auto first = kj::mv(pending.front());
    pending.pop_front();
    if (pending.empty() || first.size() >= BATCH_SIZE_BYTES) return first;

    // Concatenated ExportTraceServiceRequests join their repeated resource_spans fields.
    // Stop after reaching the threshold, exceeding it by at most one encoded request.
    kj::Vector<kj::byte> batch(first.size());
    batch.addAll(first);
    while (!pending.empty() && batch.size() < BATCH_SIZE_BYTES) {
      batch.addAll(pending.front());
      pending.pop_front();
    }
    return batch.releaseAsArray();
  }

  kj::Promise<void> run() {
    KJ_DEFER(--running);
    while (!pending.empty()) {
      auto request = takeBatch();
      try {
        co_await timer.timeoutAfter(REQUEST_TIMEOUT,
            post(KJ_ASSERT_NONNULL(collector)->start_request(kj::heap<RequestMetadata>()),
                headerTable, kj::mv(request)));
      } catch (kj::Exception& exception) {
        // Failed batches are not retried. Keep draining so a collector can recover.
        taskFailed(kj::mv(exception));
      }
    }
  }

  static kj::Promise<void> post(kj::Own<WorkerInterface> worker,
      const kj::HttpHeaderTable& headerTable,
      kj::Array<const kj::byte> request) {
    auto client = kj::newHttpClient(*worker);
    // A service takes an absolute URL; an external server sends its path and sets Host.
    co_await postOtlpTraces(
        *client, "http://localhost/v1/traces"_kj, kj::HttpHeaders(headerTable), kj::mv(request));
  }

  void taskFailed(kj::Exception&& exception) override {
    KJ_LOG(ERROR, "error exporting spans to the OTLP collector", exception);
  }
};

OtlpExporter::OtlpExporter(kj::Rc<Impl> impl): impl(kj::mv(impl)) {}

OtlpExporter::~OtlpExporter() noexcept(false) {
  impl->unlink();
}

OtlpExporter::Impl& OtlpExporter::getImpl() const {
  return *impl;
}

kj::Rc<OtlpSpanExporter> OtlpExporter::addRef() const {
  return impl.addRef();
}

kj::Own<OtlpExporter> factory_new_otlp_exporter(
    const WorkerFactory& factory, ::rust::Box<SubrequestChannel> collector) {
  return kj::heap<OtlpExporter>(kj::rc<OtlpExporter::Impl>(
      kj::mv(collector), *factory.getImpl().headerTable, factory.getImpl().timer));
}

kj::Rc<OtlpSpanExporter> otlp_exporter_add_ref(const OtlpExporter& exporter) {
  return exporter.addRef();
}

void otlp_exporter_unlink(const OtlpExporter& exporter) {
  exporter.getImpl().unlink();
}

}  // namespace workerd::server
