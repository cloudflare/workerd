#include "io-thread-context.h"

#include <capnp/schema.h>

namespace workerd {

namespace {
// The $commonText annotation in http-over-capnp.capnp, which gives each CommonHeaderName its
// header name. HttpOverCapnpFactory reads the names the same way.
constexpr uint64_t COMMON_TEXT_ANNOTATION = 0x857745131db6fc83ull;
}  // namespace

CommonHeaderIds::CommonHeaderIds(kj::HttpHeaderTable::Builder& builder) {
  auto& table = builder.getFutureTable();
  auto enumerants = capnp::Schema::from<capnp::CommonHeaderName>().getEnumerants();
  KJ_ASSERT(enumerants.size() == MAX_COMMON_HEADER_ID + 1);

  for (auto& slot: kjToCommon) slot = 0;
  commonToKj[0] = kj::HttpHeaderId::CONNECTION;  // Unused: id 0 is INVALID.
  commonNames[0] = nullptr;

  for (kj::uint i = 1; i <= MAX_COMMON_HEADER_ID; i++) {
    kj::StringPtr nameText;
    for (auto ann: enumerants[i].getProto().getAnnotations()) {
      if (ann.getId() == COMMON_TEXT_ANNOTATION) {
        nameText = ann.getValue().getText();
        break;
      }
    }
    KJ_ASSERT(nameText != nullptr);

    auto id = builder.add(nameText);
    commonToKj[i] = id;
    // The table keeps the string from whichever add() of this name came first.
    commonNames[i] = table.idToString(id).begin();
    if (id.hashCode() < KJ_ID_LIMIT) {
      kjToCommon[id.hashCode()] = i;
    }
  }
}

ThreadContext::HeaderIdBundle::HeaderIdBundle(kj::HttpHeaderTable::Builder& builder)
    : table(builder.getFutureTable()),
      contentEncoding(builder.add("Content-Encoding")),
      cfCacheStatus(builder.add("CF-Cache-Status")),
      cacheControl(builder.add("Cache-Control")),
      pragma(builder.add("Pragma")),
      cfCacheNamespace(builder.add("CF-Cache-Namespace")),
      range(builder.add("Range")),
      ifModifiedSince(builder.add("If-Modified-Since")),
      ifNoneMatch(builder.add("If-None-Match")),
      cfKvMetadata(builder.add("CF-KV-Metadata")),
      cfR2ErrorHeader(builder.add("CF-R2-Error")),
      cfBlobMetadataSize(builder.add("CF-R2-Metadata-Size")),
      cfBlobRequest(builder.add("CF-R2-Request")),
      authorization(builder.add("Authorization")),
      cfQueuesErrorCode(builder.add("CF-Queues-Error-Code")),
      cfQueuesErrorCause(builder.add("CF-Queues-Error-Cause")),
      secWebSocketProtocol(builder.add("Sec-WebSocket-Protocol")),
      userAgent(builder.add("User-Agent")),
      contentType(builder.add("Content-Type")),
      contentLength(builder.add("Content-Length")),
      accept(builder.add("Accept")),
      acceptEncoding(builder.add("Accept-Encoding")),
      cfRay(builder.add("CF-Ray")),
      origin(builder.add("Origin")),
      commonHeaderIds(builder) {}

ThreadContext::ThreadContext(kj::Timer& timer,
    kj::EntropySource& entropySource,
    HeaderIdBundle headerIds,
    capnp::HttpOverCapnpFactory& httpOverCapnpFactory,
    capnp::ByteStreamFactory& byteStreamFactory)
    : timer(timer),
      entropySource(entropySource),
      headerIds(headerIds),
      httpOverCapnpFactory(httpOverCapnpFactory),
      byteStreamFactory(byteStreamFactory) {}

}  // namespace workerd
