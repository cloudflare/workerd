// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "worker-factory-impl.h"

namespace workerd::server {

// Submitters may outlive the services, so exports run in their own task set rather than a
// worker's. Unlinking disables later exports and releases the collector's service graph.
class OtlpExporter::Impl final: public OtlpSpanExporter, private kj::TaskSet::ErrorHandler {
 public:
  Impl(::rust::Box<SubrequestChannel> collector, const kj::HttpHeaderTable& headerTable)
      : collector(kj::mv(collector)),
        headerTable(headerTable) {}

  void unlink() {
    // Cancelling requests can close spans. Disable exports before those cancellations run.
    auto collector = kj::mv(this->collector);
    this->collector = kj::none;
    tasks.clear();
    collector = kj::none;
  }

  void exportSpans(kj::Array<const kj::byte> request, uint spanCount) override {
    auto& c = KJ_UNWRAP_OR_RETURN(collector);
    tasks.add(kj::evalNow([&]() {
      return post(c->start_request(kj::heap<RequestMetadata>()), headerTable, kj::mv(request));
    }));
  }

 private:
  kj::Maybe<::rust::Box<SubrequestChannel>> collector;
  const kj::HttpHeaderTable& headerTable;
  kj::TaskSet tasks{*this};

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
  return kj::heap<OtlpExporter>(
      kj::rc<OtlpExporter::Impl>(kj::mv(collector), *factory.getImpl().headerTable));
}

kj::Rc<OtlpSpanExporter> otlp_exporter_add_ref(const OtlpExporter& exporter) {
  return exporter.addRef();
}

void otlp_exporter_unlink(const OtlpExporter& exporter) {
  exporter.getImpl().unlink();
}

}  // namespace workerd::server
