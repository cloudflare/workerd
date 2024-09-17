#pragma once

#include <capnp/compat/http-over-capnp.h>
#include <kj/compat/http.h>

namespace workerd {

// Thread-level stuff needed to construct a IoContext. One of these is created for each
// request-handling thread.
class ThreadContext {
public:
  struct HeaderIdBundle {
    HeaderIdBundle(kj::HttpHeaderTable::Builder& builder);

    const kj::HttpHeaderTable& table;

    const kj::HttpHeaderId contentEncoding;
    const kj::HttpHeaderId cfCacheStatus;  // used by cache API implementation
    const kj::HttpHeaderId cacheControl;
    const kj::HttpHeaderId cfCacheNamespace;    // used by Cache binding implementation
    const kj::HttpHeaderId cfKvMetadata;        // used by KV binding implementation
    const kj::HttpHeaderId cfR2ErrorHeader;     // used by R2 binding implementation
    const kj::HttpHeaderId cfBlobMetadataSize;  // used by R2 binding implementation
    const kj::HttpHeaderId cfBlobRequest;       // used by R2 binding implementation
    const kj::HttpHeaderId authorization;       // used by R2 binding implementation
    const kj::HttpHeaderId secWebSocketProtocol;
  };

  ThreadContext(kj::Timer& timer,
      kj::EntropySource& entropySource,
      HeaderIdBundle headerIds,
      capnp::HttpOverCapnpFactory& httpOverCapnpFactory,
      capnp::ByteStreamFactory& byteStreamFactory,
      bool isFiddle);

  // This should only be used to construct TimerChannel. Everything else should use TimerChannel.
  inline kj::Timer& getUnsafeTimer() const {
    return timer;
  }
  inline kj::EntropySource& getEntropySource() const {
    return entropySource;
  }
  inline const kj::HttpHeaderTable& getHeaderTable() const {
    return headerIds.table;
  }
  inline const HeaderIdBundle& getHeaderIds() const {
    return headerIds;
  }
  inline capnp::HttpOverCapnpFactory& getHttpOverCapnpFactory() const {
    return httpOverCapnpFactory;
  }
  inline capnp::ByteStreamFactory& getByteStreamFactory() const {
    return byteStreamFactory;
  }
  inline bool isFiddle() const {
    return fiddle;
  }

private:
  // NOTE: This timer only updates when entering the event loop!
  kj::Timer& timer;
  kj::EntropySource& entropySource;
  HeaderIdBundle headerIds;
  capnp::HttpOverCapnpFactory& httpOverCapnpFactory;
  capnp::ByteStreamFactory& byteStreamFactory;
  bool fiddle;
};

}  // namespace workerd
