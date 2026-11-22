#pragma once

#include <capnp/compat/http-over-capnp.h>
#include <kj/compat/http.h>

namespace workerd {

// Maps between capnp::CommonHeaderName and the ids that one kj::HttpHeaderTable assigns to those
// headers. api::Headers keys its storage by CommonHeaderName, so this lets headers move between
// it and kj::HttpHeaders without hashing their names again.
class CommonHeaderIds {
 public:
  static constexpr kj::uint MAX_COMMON_HEADER_ID =
      static_cast<kj::uint>(capnp::CommonHeaderName::WWW_AUTHENTICATE);

  // Registers every common header name with `builder`. HttpOverCapnpFactory::HeaderIdBundle
  // registers the same names; Builder::add() returns the existing id for a name added before.
  explicit CommonHeaderIds(kj::HttpHeaderTable::Builder& builder);

  // The id of `name` in this table. Only valid for kj::HttpHeaders built on this table.
  kj::HttpHeaderId toKj(capnp::CommonHeaderName name) const {
    return commonToKj[static_cast<kj::uint>(name)];
  }

  // The common header that `id` identifies, if any. `name` must be `id.toString()`.
  //
  // `id` may come from a different table: kj::HttpHeaderId doesn't expose its table, so this
  // checks that `id`'s table holds the very same name string under that id as ours does, which
  // means the two agree on what the id names. Otherwise it returns none and the caller has to
  // look the header up by name.
  kj::Maybe<capnp::CommonHeaderName> fromKj(kj::HttpHeaderId id, kj::StringPtr name) const {
    auto index = id.hashCode();
    if (index >= kjToCommon.size()) return kj::none;
    kj::uint common = kjToCommon[index];
    if (common == 0 || name.begin() != commonNames[common]) return kj::none;
    return static_cast<capnp::CommonHeaderName>(common);
  }

 private:
  // Ids past this limit just miss the fast path in fromKj(). Tables register the common headers
  // early, so their ids are far smaller than this in practice.
  static constexpr kj::uint KJ_ID_LIMIT = 256;

  kj::FixedArray<kj::HttpHeaderId, MAX_COMMON_HEADER_ID + 1> commonToKj;
  // The name string the table stores for each common header's id (see fromKj()).
  kj::FixedArray<const char*, MAX_COMMON_HEADER_ID + 1> commonNames;
  // Indexed by HttpHeaderId::hashCode(); zero means not a common header.
  kj::FixedArray<uint8_t, KJ_ID_LIMIT> kjToCommon;
};

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
    const kj::HttpHeaderId pragma;
    const kj::HttpHeaderId cfCacheNamespace;  // used by Cache binding implementation
    const kj::HttpHeaderId range;
    const kj::HttpHeaderId ifModifiedSince;
    const kj::HttpHeaderId ifNoneMatch;
    const kj::HttpHeaderId cfKvMetadata;        // used by KV binding implementation
    const kj::HttpHeaderId cfR2ErrorHeader;     // used by R2 binding implementation
    const kj::HttpHeaderId cfBlobMetadataSize;  // used by R2 binding implementation
    const kj::HttpHeaderId cfBlobRequest;       // used by R2 binding implementation
    const kj::HttpHeaderId authorization;       // used by R2 binding implementation
    const kj::HttpHeaderId cfQueuesErrorCode;   // used by Queue binding implementation
    const kj::HttpHeaderId cfQueuesErrorCause;  // used by Queue binding implementation
    const kj::HttpHeaderId secWebSocketProtocol;
    const kj::HttpHeaderId userAgent;       // used in tracing instrumentation
    const kj::HttpHeaderId contentType;     // used in tracing instrumentation
    const kj::HttpHeaderId contentLength;   // used in tracing instrumentation
    const kj::HttpHeaderId accept;          // used in tracing instrumentation
    const kj::HttpHeaderId acceptEncoding;  // used in tracing instrumentation
    const kj::HttpHeaderId cfRay;           // used in tracing instrumentation
    const kj::HttpHeaderId origin;

    const CommonHeaderIds commonHeaderIds;
  };

  ThreadContext(kj::Timer& timer,
      kj::EntropySource& entropySource,
      HeaderIdBundle headerIds,
      capnp::HttpOverCapnpFactory& httpOverCapnpFactory,
      capnp::ByteStreamFactory& byteStreamFactory);

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

 private:
  // NOTE: This timer only updates when entering the event loop!
  kj::Timer& timer;
  kj::EntropySource& entropySource;
  HeaderIdBundle headerIds;
  capnp::HttpOverCapnpFactory& httpOverCapnpFactory;
  capnp::ByteStreamFactory& byteStreamFactory;
};

}  // namespace workerd
