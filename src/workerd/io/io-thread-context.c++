#include "io-thread-context.h"

namespace workerd {

ThreadContext::HeaderIdBundle::HeaderIdBundle(kj::HttpHeaderTable::Builder& builder)
    : table(builder.getFutureTable()),
      contentEncoding(builder.add("Content-Encoding")),
      cfCacheStatus(builder.add("CF-Cache-Status")),
      cacheControl(builder.add("Cache-Control")),
      pragma(builder.add("Pragma")),
      cfCacheNamespace(builder.add("CF-Cache-Namespace")),
      cfKvMetadata(builder.add("CF-KV-Metadata")),
      cfR2ErrorHeader(builder.add("CF-R2-Error")),
      cfBlobMetadataSize(builder.add("CF-R2-Metadata-Size")),
      cfBlobRequest(builder.add("CF-R2-Request")),
      authorization(builder.add("Authorization")),
      secWebSocketProtocol(builder.add("Sec-WebSocket-Protocol")) {}

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
