#include "io-thread-context.h"

namespace workerd {

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
      secWebSocketProtocol(builder.add("Sec-WebSocket-Protocol")),
      userAgent(builder.add("User-Agent")),
      contentType(builder.add("Content-Type")),
      contentLength(builder.add("Content-Length")),
      accept(builder.add("Accept")),
      acceptEncoding(builder.add("Accept-Encoding")),
      cfRay(builder.add("CF-Ray")) {}

ThreadContext::ThreadContext(kj::Timer& timer,
    kj::EntropySource& entropySource,
    HeaderIdBundle headerIds,
    capnp::HttpOverCapnpFactory& httpOverCapnpFactory,
    capnp::ByteStreamFactory& byteStreamFactory,
    bool fiddle)
    : timer(timer),
      entropySource(entropySource),
      headerIds(headerIds),
      httpOverCapnpFactory(httpOverCapnpFactory),
      byteStreamFactory(byteStreamFactory),
      fiddle(fiddle) {}

}  // namespace workerd
