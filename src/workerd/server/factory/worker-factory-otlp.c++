// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "otlp-export-queue.h"
#include "worker-factory-impl.h"

namespace workerd::server {

namespace {

class CollectorTransport final: public OtlpExportQueue::Transport {
 public:
  CollectorTransport(
      ::rust::Box<SubrequestChannel> collector, const kj::HttpHeaderTable& headerTable)
      : collector(kj::mv(collector)),
        headerTable(headerTable) {}

  kj::Promise<void> post(kj::Array<const kj::byte> request) override {
    auto worker = collector->start_request(kj::heap<RequestMetadata>());
    auto client = kj::newHttpClient(*worker);
    // A service takes an absolute URL; an external server sends its path and sets Host.
    co_await postOtlpTraces(
        *client, "http://localhost/v1/traces"_kj, kj::HttpHeaders(headerTable), kj::mv(request));
  }

 private:
  ::rust::Box<SubrequestChannel> collector;
  const kj::HttpHeaderTable& headerTable;
};

}  // namespace

// Submitters may outlive the services, so exports run in their own task set rather than a
// worker's. Unlinking disables later exports and releases the collector's service graph.
class OtlpExporter::Impl final: public OtlpSpanExporter {
 public:
  Impl(::rust::Box<SubrequestChannel> collector,
      const kj::HttpHeaderTable& headerTable,
      kj::Timer& timer)
      : queue(kj::heap<CollectorTransport>(kj::mv(collector), headerTable), timer) {}

  void unlink() {
    queue.unlink();
  }

  void exportSpans(kj::Array<const kj::byte> request, uint spanCount) override {
    queue.exportSpans(kj::mv(request), spanCount);
  }

 private:
  OtlpExportQueue queue;
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
