// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "otlp-export-queue.h"
#include "worker-factory-impl.h"

namespace workerd::server {

namespace {

class CollectorTransport final: public OtlpExportQueue::Transport {
 public:
  // `url` is `/v1/traces` or `/v1/logs` under the collector's path.
  CollectorTransport(::rust::Box<SubrequestChannel> collector,
      const kj::HttpHeaderTable& headerTable,
      kj::String url)
      : collector(kj::mv(collector)),
        headerTable(headerTable),
        url(kj::mv(url)) {}

  kj::Promise<void> post(kj::Array<const kj::byte> request) override {
    auto worker = collector->start_request(kj::heap<RequestMetadata>());
    auto client = kj::newHttpClient(*worker);
    // A service takes an absolute URL; an external server sends its path and sets Host.
    co_await postOtlpTraces(*client, url, kj::HttpHeaders(headerTable), kj::mv(request));
  }

 private:
  ::rust::Box<SubrequestChannel> collector;
  const kj::HttpHeaderTable& headerTable;
  kj::String url;
};

kj::Own<OtlpExportQueue> collectorQueue(::rust::Box<SubrequestChannel> collector,
    const kj::HttpHeaderTable& headerTable,
    kj::Timer& timer,
    ::rust::Str path,
    kj::StringPtr signal) {
  auto url = kj::str(
      "http://localhost", kj::ArrayPtr<const char>(path.data(), path.size()), "/v1/", signal);
  return kj::heap<OtlpExportQueue>(
      kj::heap<CollectorTransport>(kj::mv(collector), headerTable, kj::mv(url)), timer);
}

}  // namespace

// Submitters may outlive the services, so exports run in their own task set rather than a
// worker's. Unlinking disables later exports and releases the collector's service graph.
class OtlpExporter::Impl final: public OtlpSpanExporter {
 public:
  // Each signal has its own queue: a queue joins requests by concatenating them, which only
  // works for requests of one message type.
  Impl(kj::Own<OtlpExportQueue> queue, kj::Maybe<kj::Own<OtlpExportQueue>> logsQueue)
      : queue(kj::mv(queue)),
        logsQueue(kj::mv(logsQueue)) {}
  explicit Impl(::rust::Box<TelemetrySink> sink): sink(kj::mv(sink)) {}

  void unlink() {
    KJ_IF_SOME(q, queue) {
      q->unlink();
    }
    KJ_IF_SOME(q, logsQueue) {
      q->unlink();
    }
    unlinked = true;
  }

  bool exportsLogs() const {
    return sink != kj::none || logsQueue != kj::none;
  }

  void exportSpans(kj::Array<const kj::byte> request, uint spanCount) override {
    KJ_IF_SOME(s, sink) {
      if (!unlinked)
        telemetry_spans(*s, ::rust::Slice<const kj::byte>(request.begin(), request.size()));
    }
    KJ_IF_SOME(q, queue) {
      q->exportSpans(kj::mv(request), spanCount);
    }
  }

  void exportLogs(kj::Array<const kj::byte> request, uint recordCount) override {
    KJ_IF_SOME(s, sink) {
      if (!unlinked)
        telemetry_logs(*s, ::rust::Slice<const kj::byte>(request.begin(), request.size()));
    }
    KJ_IF_SOME(q, logsQueue) {
      q->exportSpans(kj::mv(request), recordCount);
    }
  }

 private:
  kj::Maybe<kj::Own<OtlpExportQueue>> queue;
  kj::Maybe<kj::Own<OtlpExportQueue>> logsQueue;
  kj::Maybe<::rust::Box<TelemetrySink>> sink;
  bool unlinked = false;
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

kj::Own<OtlpExporter> factory_new_otlp_exporter(const WorkerFactory& factory,
    ::rust::Box<SubrequestChannel> traces,
    ::rust::Box<SubrequestChannel> logs,
    ::rust::Str path,
    bool exportLogs) {
  auto& impl = factory.getImpl();
  kj::Maybe<kj::Own<OtlpExportQueue>> logsQueue;
  if (exportLogs) {
    logsQueue = collectorQueue(kj::mv(logs), *impl.headerTable, impl.timer, path, "logs"_kj);
  }
  return kj::heap<OtlpExporter>(kj::rc<OtlpExporter::Impl>(
      collectorQueue(kj::mv(traces), *impl.headerTable, impl.timer, path, "traces"_kj),
      kj::mv(logsQueue)));
}

kj::Own<OtlpExporter> factory_new_bucket_exporter(
    const WorkerFactory& factory, ::rust::Box<TelemetrySink> sink) {
  return kj::heap<OtlpExporter>(kj::rc<OtlpExporter::Impl>(kj::mv(sink)));
}

bool otlp_exporter_exports_logs(const OtlpExporter& exporter) {
  return exporter.getImpl().exportsLogs();
}

kj::Rc<OtlpSpanExporter> otlp_exporter_add_ref(const OtlpExporter& exporter) {
  return exporter.addRef();
}

void otlp_exporter_unlink(const OtlpExporter& exporter) {
  exporter.getImpl().unlink();
}

}  // namespace workerd::server
