// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "otlp-export-queue.h"
#include "worker-factory-impl.h"

#include <workerd/io/otlp-span-submitter.h>

namespace workerd::server {

namespace {

// How long a request waits to be batched with later ones, so that a collector writing a file per
// POST, as the bundled telemetry Worker does, writes about one a second per signal.
constexpr kj::Duration LINGER = kj::SECONDS;

// The collector's channel, shared by the traces and logs queues' transports.
struct Collector final: public kj::Refcounted {
  explicit Collector(::rust::Box<SubrequestChannel> channel): channel(kj::mv(channel)) {}
  ::rust::Box<SubrequestChannel> channel;
};

class CollectorTransport final: public OtlpExportQueue::Transport {
 public:
  // `url` is `/v1/traces` or `/v1/logs` under the collector's path. With `json`, each batch is
  // re-encoded as OTLP/JSON.
  CollectorTransport(kj::Rc<Collector> collector,
      const kj::HttpHeaderTable& headerTable,
      kj::String url,
      bool logs,
      bool json)
      : collector(kj::mv(collector)),
        headerTable(headerTable),
        url(kj::mv(url)),
        logs(logs),
        json(json) {}

  kj::Promise<void> post(kj::Array<const kj::byte> request) override {
    auto contentType = "application/x-protobuf"_kj;
    if (json) {
      auto body = rust::otlp::encode_json(
          logs, ::rust::Slice<const kj::byte>(request.begin(), request.size()));
      request = kj::heapArray<const kj::byte>(body.data(), body.size());
      contentType = "application/json"_kj;
    }
    auto worker = collector->channel->start_request(kj::heap<RequestMetadata>());
    auto client = kj::newHttpClient(*worker);
    // A service takes an absolute URL; an external server sends its path and sets Host.
    co_await postOtlpTraces(
        *client, url, kj::HttpHeaders(headerTable), kj::mv(request), contentType);
  }

 private:
  kj::Rc<Collector> collector;
  const kj::HttpHeaderTable& headerTable;
  kj::String url;
  bool logs;
  bool json;
};

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

  void unlink() {
    queue->unlink();
    KJ_IF_SOME(q, logsQueue) {
      q->unlink();
    }
  }

  void exportSpans(kj::Array<const kj::byte> request, uint spanCount) override {
    queue->exportSpans(kj::mv(request), spanCount);
  }

  void exportLogs(kj::Array<const kj::byte> request, uint recordCount) override {
    KJ_IF_SOME(q, logsQueue) {
      q->exportSpans(kj::mv(request), recordCount);
    }
  }

 private:
  kj::Own<OtlpExportQueue> queue;
  kj::Maybe<kj::Own<OtlpExportQueue>> logsQueue;
};

OtlpExporter::OtlpExporter(kj::Rc<Impl> impl, bool exportsLogs, double sampleRatio)
    : exportsLogs(exportsLogs),
      sampleRatio(sampleRatio),
      impl(kj::mv(impl)) {}

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
    ::rust::Box<SubrequestChannel> collector,
    ::rust::Str path,
    bool exportLogs,
    bool json,
    double sampleRatio) {
  auto& impl = factory.getImpl();
  auto shared = kj::rc<Collector>(kj::mv(collector));
  auto queue = [&](bool logs) {
    auto url = kj::str("http://localhost", kj::ArrayPtr<const char>(path.data(), path.size()),
        logs ? "/v1/logs"_kj : "/v1/traces"_kj);
    return kj::heap<OtlpExportQueue>(
        kj::heap<CollectorTransport>(shared.addRef(), *impl.headerTable, kj::mv(url), logs, json),
        impl.timer, LINGER);
  };
  kj::Maybe<kj::Own<OtlpExportQueue>> logsQueue;
  if (exportLogs) logsQueue = queue(true);
  return kj::heap<OtlpExporter>(
      kj::rc<OtlpExporter::Impl>(queue(false), kj::mv(logsQueue)), exportLogs, sampleRatio);
}

kj::Rc<OtlpSpanExporter> otlp_exporter_add_ref(const OtlpExporter& exporter) {
  return exporter.addRef();
}

void otlp_exporter_unlink(const OtlpExporter& exporter) {
  exporter.getImpl().unlink();
}

}  // namespace workerd::server
