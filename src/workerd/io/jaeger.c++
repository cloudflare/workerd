// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "jaeger.h"
#include <workerd/io/jaeger-model.pb.h>
#include <kj/debug.h>
#include <kj/vector.h>
#include <cstdlib>
#include <workerd/util/thread-scopes.h>

namespace workerd {

using kj::uint;

namespace {

kj::Maybe<uint> tryFromHexDigit(char c) {
  if ('0' <= c && c <= '9') {
    return c - '0';
  } else if ('a' <= c && c <= 'f') {
    return c - ('a' - 10);
  } else if ('A' <= c && c <= 'F') {
    return c - ('A' - 10);
  } else {
    return nullptr;
  }
}

kj::Maybe<uint64_t> hexToUint64(kj::ArrayPtr<const char> s) {
  KJ_ASSERT(s.size() <= 16);
  uint64_t value = 0;
  for (auto ch: s) {
    KJ_IF_MAYBE(d, tryFromHexDigit(ch)) {
      value = (value << 4) + *d;
    } else {
      return nullptr;
    }
  }
  return value;
}

void addHex(kj::Vector<char>& out, uint64_t v) {
  const char HEX_DIGITS[] = "0123456789abcdef";
  for (int i = 0; i < 16; ++i) {
    out.add(HEX_DIGITS[v >> (64 - 4)]);
    v = v << 4;
  }
};

void addBigEndianBytes(kj::Vector<byte>& out, uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    out.add(v >> (64 - 8));
    v = v << 8;
  }
};

template <typename T>
void setJaegerTimeField(T& timeField, kj::Duration value) {
  // Set either a google::protobuf::Timestamp or ::Duration from a kj::Duration. If setting a
  // Timestamp, the kj::Duration should be relative to the Unix epoch.

  // We use microseconds for consistency with Jaeger Thrift format, even though its protobuf format
  // could do nanos.
  uint64_t micros = value / kj::MICROSECONDS;
  timeField.set_seconds(micros / 1000000);
  timeField.set_nanos(1000 * (micros % 1000000));
}

template <typename T>
void setJaegerTimeField(T& timeField, kj::Date value) {
  return setJaegerTimeField(timeField, value - kj::UNIX_EPOCH);
}

void setJaegerTag(jaeger::api_v2::KeyValue& kv, const Span::Tag& tag) {
  kv.set_key(tag.key.begin(), tag.key.size());
  KJ_SWITCH_ONEOF(tag.value) {
    KJ_CASE_ONEOF(b, bool) {
      kv.set_v_type(::jaeger::api_v2::BOOL);
      kv.set_v_bool(b);
    }
    KJ_CASE_ONEOF(i, int64_t) {
      kv.set_v_type(::jaeger::api_v2::INT64);
      kv.set_v_int64(i);
    }
    KJ_CASE_ONEOF(d, double) {
      kv.set_v_type(::jaeger::api_v2::FLOAT64);
      kv.set_v_float64(d);
    }
    KJ_CASE_ONEOF(s, kj::String) {
      kv.set_v_type(::jaeger::api_v2::STRING);
      kv.set_v_str(s.begin(), s.size());
    }
  }
}

void setJaegerLog(jaeger::api_v2::Log& l, const Span::Log& log) {
  setJaegerTimeField(*l.mutable_timestamp(), log.timestamp);
  setJaegerTag(*l.add_fields(), log.tag);
}

} // namespace

kj::Maybe<Jaeger::TraceId> Jaeger::TraceId::fromGoString(kj::ArrayPtr<const char> s) {
  // Reference: https://github.com/jaegertracing/jaeger/blob/e46f8737/model/ids.go#L58
  auto n = s.size();
  if (n > 32) {
    return nullptr;
  } else if (n <= 16) {
    KJ_IF_MAYBE(low, hexToUint64(s)) {
      return Jaeger::TraceId(*low, 0);
    }
  } else {
    KJ_IF_MAYBE(high, hexToUint64(s.slice(0, n - 16))) {
      KJ_IF_MAYBE(low, hexToUint64(s.slice(n - 16, n))) {
        return Jaeger::TraceId(*low, *high);
      }
    }
  }
  return nullptr;
}

kj::String Jaeger::TraceId::toGoString() const {
  // Reference: https://github.com/jaegertracing/jaeger/blob/e46f8737/model/ids.go#L50
  if (high == 0) {
    kj::Vector<char> s(17);
    addHex(s, low);
    s.add('\0');
    return kj::String(s.releaseAsArray());
  } else {
    kj::Vector<char> s(33);
    addHex(s, high);
    addHex(s, low);
    s.add('\0');
    return kj::String(s.releaseAsArray());
  }
}

kj::Maybe<Jaeger::TraceId> Jaeger::TraceId::fromProtobuf(kj::ArrayPtr<byte> buf) {
  // Reference: https://github.com/jaegertracing/jaeger/blob/e46f8737/model/ids.go#L111
  if (buf.size() != 16) {
    return nullptr;
  }
  uint64_t high = 0;
  for (auto i: kj::zeroTo(8)) {
    high = (high << 8) + buf[i];
  }
  uint64_t low = 0;
  for (auto i: kj::zeroTo(8)) {
    low = (low << 8) + buf[i + 8];
  }
  return Jaeger::TraceId(low, high);
}

kj::Array<byte> Jaeger::TraceId::toProtobuf() const {
  // Reference: https://github.com/jaegertracing/jaeger/blob/e46f8737/model/ids.go#L81
  kj::Vector<byte> s(16);
  addBigEndianBytes(s, high);
  addBigEndianBytes(s, low);
  return s.releaseAsArray();
}

kj::Maybe<Jaeger::SpanId> Jaeger::SpanId::fromGoString(kj::ArrayPtr<const char> s) {
  // Reference: https://github.com/jaegertracing/jaeger/blob/e46f8737/model/ids.go#L170
  auto n = s.size();
  if (n > 16) {
    return nullptr;
  } else {
    KJ_IF_MAYBE(value, hexToUint64(s)) {
      return Jaeger::SpanId(*value);
    }
  }
  return nullptr;
}

kj::String Jaeger::SpanId::toGoString() const {
  // Reference: https://github.com/jaegertracing/jaeger/blob/e46f8737/model/ids.go#L165
  kj::Vector<char> s(17);
  addHex(s, value);
  s.add('\0');
  return kj::String(s.releaseAsArray());
}

kj::Maybe<Jaeger::SpanId> Jaeger::SpanId::fromProtobuf(kj::ArrayPtr<byte> buf) {
  // Reference: https://github.com/jaegertracing/jaeger/blob/e46f8737/model/ids.go#L182
  if (buf.size() != 8) {
    return nullptr;
  }
  uint64_t value = 0;
  for (auto i: kj::zeroTo(8)) {
    value = (value << 8) + buf[i];
  }
  return Jaeger::SpanId(value);

}

kj::Array<byte> Jaeger::SpanId::toProtobuf() const {
  // Reference: https://github.com/jaegertracing/jaeger/blob/e46f8737/model/ids.go#L81
  kj::Vector<byte> s(8);
  addBigEndianBytes(s, value);
  return s.releaseAsArray();
}

kj::Maybe<Jaeger::SpanContext> Jaeger::SpanContext::fromHeader(kj::StringPtr header) {
  kj::StringPtr remaining = header;
  auto nextToken = [&]() -> kj::ArrayPtr<const char> {
    const char* pos = strchr(remaining.begin(), ':');
    if (pos == nullptr) {
      return nullptr;
    }
    auto token = kj::ArrayPtr<const char>(remaining.begin(), pos);
    remaining = kj::StringPtr(pos + 1, remaining.end());
    return token;
  };

  KJ_IF_MAYBE(traceId, TraceId::fromGoString(nextToken())) {
    KJ_IF_MAYBE(spanId, SpanId::fromGoString(nextToken())) {
      KJ_IF_MAYBE(parentSpanId, SpanId::fromGoString(nextToken())) {
        char* end;
        // TODO(conform): technically, allows reinterpreted negatives:
        uint flags = strtoul(remaining.begin(), &end, 10);
        if (end > remaining.begin() && end == remaining.end()) {
          return SpanContext(*traceId, *spanId, *parentSpanId, flags);
        }
      }
    }
  }

  return nullptr;
}

kj::Maybe<Jaeger::SpanContext> Jaeger::SpanContext::fromParent(SpanParent& parent) {
  return parent.getObserver().map([](SpanObserver& observer) {
    if (Observer* jaegerObserver = dynamic_cast<Observer*>(&observer)) {
      return jaegerObserver->getContext();
    } else {
      KJ_FAIL_REQUIRE("tried to extract Jaeger SpanContext from unknown observer type");
    }
  });
}

kj::String Jaeger::SpanContext::toHeader() const {
  return kj::str(traceId, ":", spanId, ":", parentSpanId, ":", flags);
}

kj::Array<byte> Jaeger::spanToProtobuf(const SpanContext& context, const Span& span,
                                       kj::ArrayPtr<Span::Tag> processTags,
                                       kj::ArrayPtr<Span::Tag> defaultTags,
                                       kj::StringPtr serviceName) {
  auto traceIdBuf = context.traceId.toProtobuf();
  auto spanIdBuf = context.spanId.toProtobuf();
  auto parentSpanIdBuf = context.parentSpanId.toProtobuf();

  jaeger::api_v2::Span s;
  s.mutable_process()->set_service_name(serviceName);
  // TODO(soon): use (cordon-suffixed) executable name instead?

  for (auto& tag: processTags) {
    setJaegerTag(*s.mutable_process()->add_tags(), tag);
  }

  for (auto& tag: defaultTags) {
    // Don't override the span's own tags.
    if (span.tags.find(tag.key) == nullptr) {
      setJaegerTag(*s.add_tags(), tag);
    }
  }

  for (auto& tag: span.tags) {
    setJaegerTag(*s.add_tags(), tag);
  }

  for (auto& log: span.logs) {
    setJaegerLog(*s.add_logs(), log);
  }

  s.set_trace_id(traceIdBuf.begin(), traceIdBuf.size());
  s.set_span_id(spanIdBuf.begin(), spanIdBuf.size());
  s.set_flags(context.flags);

  std::string operationNameStr(span.operationName.begin(), span.operationName.size());
  s.set_operation_name(operationNameStr);

  if (isPredictableModeForTest()) {
    // Initialize these to empty values.
    s.mutable_start_time();
    s.mutable_duration();
  } else {
    setJaegerTimeField(*s.mutable_start_time(), span.startTime);
    setJaegerTimeField(*s.mutable_duration(), span.endTime - span.startTime);
  }

  jaeger::api_v2::SpanRef *parent = s.add_references();
  parent->set_trace_id(traceIdBuf.begin(), traceIdBuf.size());
  parent->set_span_id(parentSpanIdBuf.begin(), parentSpanIdBuf.size());
  parent->set_ref_type(jaeger::api_v2::CHILD_OF);

  auto jaegerProtoBuf = kj::heapArray<byte>(s.ByteSizeLong());
  s.SerializeToArray(jaegerProtoBuf.begin(), jaegerProtoBuf.size());

  return jaegerProtoBuf;
}

kj::String KJ_STRINGIFY(const Jaeger::TraceId& t) {
  return t.toGoString();
}

kj::String KJ_STRINGIFY(const Jaeger::SpanId& s) {
  return s.toGoString();
}

kj::String KJ_STRINGIFY(const Jaeger::SpanContext& s) {
  return s.toHeader();
}

kj::Own<SpanObserver> Jaeger::Observer::newChild() {
  return kj::refcounted<Observer>(kj::addRef(*submitter),
      SpanContext(context.traceId, submitter->makeSpanId(), context.spanId, context.flags));
}

void Jaeger::Observer::report(const Span& span) {
  submitter->submitSpan(context, span);
}

} // namespace workerd
