// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once
// Similar to jaeger-model.proto, the code in this file relates to Cloudflare Workers' internal
// tracing infrastructure, which uses Jaeger. This code is not used by `workerd` but is currenly
// included because it is a dependency of the common trace APIs that also implement Trace Workers.
// Eventually we would like to properly abstract trace collection and remove the Jaeger-specific
// parts (or maybe make them available as an indepnedent library?). Long-term the right way for
// `workerd` users to do tracing is through Trace Workers, which can integrate with arbitrary
// tracing systems.

#include <cstdint>
#include <kj/string.h>
#include <kj/time.h>
#include <kj/map.h>
#include <kj/one-of.h>
#include <workerd/io/jaeger.capnp.h>

namespace workerd {

using kj::byte;
using kj::uint;

// TODO(cleanup) - better as namespace instead of class?
class Jaeger {
public:
  struct TraceId {
    // A 128-bit globally unique trace identifier.
    explicit TraceId(uint64_t low, uint64_t high) : low(low), high(high) {}

    static kj::Maybe<TraceId> fromGoString(kj::ArrayPtr<const char> s);
    kj::String toGoString() const;
    // Replicates Jaeger go library's string serialization.

    static kj::Maybe<TraceId> fromProtobuf(kj::ArrayPtr<byte> buf);
    kj::Array<byte> toProtobuf() const;
    // Replicates Jaeger go library's protobuf serialization.

    bool operator==(const TraceId& other) const { return low == other.low && high == other.high; }
    inline bool operator!=(const TraceId& other) const { return !operator==(other); }

    // TODO(cleanup): clang has a non-standard __uint128_t?
    uint64_t low = 0;
    uint64_t high = 0;
  };

  struct SpanId {
    // A 64-bit trace-unique span identifier.
    // TODO(cleanup): Struct worth it for single value field?
    explicit SpanId(uint64_t value) : value(value) {}

    static kj::Maybe<SpanId> fromGoString(kj::ArrayPtr<const char> s);
    kj::String toGoString() const;
    // Replicates Jaeger go library's string serialization.

    static kj::Maybe<SpanId> fromProtobuf(kj::ArrayPtr<byte> buf);
    kj::Array<byte> toProtobuf() const;
    // Replicates Jaeger go library's protobuf serialization.

    bool operator==(const SpanId& other) const { return value == other.value; }
    inline bool operator!=(const SpanId& other) const { return !operator==(other); }

    uint64_t value = 0;
  };

  struct SpanContext {
    // Span meta-description, as encoded in Jaeger header

    explicit SpanContext(TraceId traceId, SpanId spanId, SpanId parentSpanId, uint flags)
        : traceId(traceId), spanId(spanId), parentSpanId(parentSpanId), flags(flags) {}

    static kj::Maybe<SpanContext> fromHeader(kj::StringPtr header);
    kj::String toHeader() const;
    // Handles colon-separated HTTP header format

    void toCapnp(rpc::JaegerSpan::Builder builder) {
      builder.setTraceIdHigh(traceId.high);
      builder.setTraceIdLow(traceId.low);
      builder.setSpanId(spanId.value);
      builder.setParentSpanId(parentSpanId.value);
      builder.setFlags(flags);
    }
    static SpanContext fromCapnp(rpc::JaegerSpan::Reader reader) {
      auto traceId = Jaeger::TraceId(reader.getTraceIdLow(), reader.getTraceIdHigh());
      auto spanId = Jaeger::SpanId(reader.getSpanId());
      auto parentSpanId = Jaeger::SpanId(reader.getParentSpanId());
      return Jaeger::SpanContext(traceId, spanId, parentSpanId, reader.getFlags());
    }

    bool operator==(const SpanContext& other) const {
      return traceId == other.traceId
          && spanId == other.spanId
          && parentSpanId == other.parentSpanId
          && flags == other.flags;
    }
    inline bool operator!=(const SpanContext& other) const { return !operator==(other); }

    TraceId traceId;
    SpanId spanId;
    SpanId parentSpanId;
    uint flags = 0;
  };

  struct SpanData {
    explicit SpanData(SpanContext context, kj::StringPtr operationName, kj::Date startTime)
        : context(context), operationName(operationName), startTime(startTime) {}
    SpanData(SpanData&&) = default;
    KJ_DISALLOW_COPY(SpanData);

    using TagValue = kj::OneOf<bool, int64_t, double, kj::String>;
    // TODO(someday): Support binary bytes, too.
    using TagMap = kj::HashMap<kj::StringPtr, TagValue>;
    using Tag = TagMap::Entry;
    // Tag and log keys are are expected to be static strings.

    kj::Array<byte> toProtobuf(kj::ArrayPtr<Tag> processTags, kj::ArrayPtr<Tag> defaultTags,
                               kj::StringPtr serviceName) const;
    // Replicates Jaeger go library's protobuf serialization.

    SpanContext context;

    kj::StringPtr operationName;
    kj::Date startTime;
    kj::Duration duration = 0 * kj::SECONDS;

    struct Log {
      kj::Date timestamp;
      Tag tag;
    };

    TagMap tags;
    kj::Vector<Log> logs;

    // TODO(someday): warnings?
  };
};

kj::String KJ_STRINGIFY(const Jaeger::TraceId& t);
kj::String KJ_STRINGIFY(const Jaeger::SpanId& t);
kj::String KJ_STRINGIFY(const Jaeger::SpanContext& t);

} // namespace workerd
