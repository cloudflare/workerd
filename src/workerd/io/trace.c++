// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/io/trace.h>
#include <workerd/util/entropy.h>
#include <workerd/util/thread-scopes.h>

#include <capnp/message.h>
#include <capnp/schema.h>
#include <kj/compat/http.h>
#include <kj/debug.h>
#include <kj/time.h>

#include <cstdlib>

namespace workerd {

namespace tracing {
namespace {
kj::Maybe<kj::uint> tryFromHexDigit(char c) {
  if ('0' <= c && c <= '9') {
    return c - '0';
  } else if ('a' <= c && c <= 'f') {
    return c - ('a' - 10);
  } else if ('A' <= c && c <= 'F') {
    return c - ('A' - 10);
  } else {
    return kj::none;
  }
}

kj::Maybe<uint64_t> hexToUint64(kj::ArrayPtr<const char> s) {
  KJ_ASSERT(s.size() <= 16);
  uint64_t value = 0;
  for (auto ch: s) {
    KJ_IF_SOME(d, tryFromHexDigit(ch)) {
      value = (value << 4) + d;
    } else {
      return kj::none;
    }
  }
  return value;
}

void addHex(kj::Vector<char>& out, uint64_t v) {
  constexpr char HEX_DIGITS[] = "0123456789abcdef";
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
}  // namespace

// Reference: https://github.com/jaegertracing/jaeger/blob/e46f8737/model/ids.go#L58
kj::Maybe<TraceId> TraceId::fromGoString(kj::ArrayPtr<const char> s) {
  auto n = s.size();
  if (n > 32) {
    return kj::none;
  } else if (n <= 16) {
    KJ_IF_SOME(low, hexToUint64(s)) {
      return TraceId(low, 0);
    }
  } else {
    KJ_IF_SOME(high, hexToUint64(s.slice(0, n - 16))) {
      KJ_IF_SOME(low, hexToUint64(s.slice(n - 16, n))) {
        return TraceId(low, high);
      }
    }
  }
  return kj::none;
}

// Reference: https://github.com/jaegertracing/jaeger/blob/e46f8737/model/ids.go#L50
kj::String TraceId::toGoString() const {
  if (high == 0) {
    kj::Vector<char> s(17);
    addHex(s, low);
    s.add('\0');
    return kj::String(s.releaseAsArray());
  }
  kj::Vector<char> s(33);
  addHex(s, high);
  addHex(s, low);
  s.add('\0');
  return kj::String(s.releaseAsArray());
}

// Reference: https://github.com/jaegertracing/jaeger/blob/e46f8737/model/ids.go#L111
kj::Maybe<TraceId> TraceId::fromProtobuf(kj::ArrayPtr<const byte> buf) {
  if (buf.size() != 16) {
    return kj::none;
  }
  uint64_t high = 0;
  for (auto i: kj::zeroTo(8)) {
    high = (high << 8) + buf[i];
  }
  uint64_t low = 0;
  for (auto i: kj::zeroTo(8)) {
    low = (low << 8) + buf[i + 8];
  }
  return TraceId(low, high);
}

// Reference: https://github.com/jaegertracing/jaeger/blob/e46f8737/model/ids.go#L81
kj::Array<byte> TraceId::toProtobuf() const {
  kj::Vector<byte> s(16);
  addBigEndianBytes(s, high);
  addBigEndianBytes(s, low);
  return s.releaseAsArray();
}

// Reference https://www.w3.org/TR/trace-context/#trace-id
kj::String TraceId::toW3C() const {
  kj::Vector<char> s(32);
  addHex(s, high);
  addHex(s, low);
  return kj::str(s.releaseAsArray());
}

namespace {
uint64_t getRandom64Bit(const kj::Maybe<kj::EntropySource&>& entropySource) {
  uint64_t ret = 0;
  uint8_t tries = 0;

  do {
    tries++;
    KJ_IF_SOME(entropy, entropySource) {
      entropy.generate(kj::asBytes(ret));
    } else {
      getEntropy(kj::asBytes(ret));
    }
    // On the extreme off chance that we ended with with zeroes
    // let's try again, but only up to three times.
  } while (ret == 0 && tries < 3);

  return ret;
}
}  // namespace

TraceId TraceId::fromEntropy(kj::Maybe<kj::EntropySource&> entropySource) {
  if (isPredictableModeForTest()) {
    return TraceId(staticSpanId, staticSpanId);
  }

  return TraceId(getRandom64Bit(entropySource), getRandom64Bit(entropySource));
}

kj::String SpanId::toGoString() const {
  kj::Vector<char> s(16);
  addHex(s, id);
  s.add('\0');
  return kj::String(s.releaseAsArray());
}

SpanId SpanId::fromEntropy(kj::Maybe<kj::EntropySource&> entropySource) {
  return SpanId(getRandom64Bit(entropySource));
}

kj::String KJ_STRINGIFY(const SpanId& id) {
  return id;
}

kj::String KJ_STRINGIFY(const TraceId& id) {
  return id;
}

InvocationSpanContext::InvocationSpanContext(kj::Badge<InvocationSpanContext>,
    kj::Maybe<kj::EntropySource&> entropySource,
    TraceId traceId,
    TraceId invocationId,
    SpanId spanId,
    kj::Maybe<const InvocationSpanContext&> parentSpanContext)
    : entropySource(entropySource),
      traceId(kj::mv(traceId)),
      invocationId(kj::mv(invocationId)),
      spanId(kj::mv(spanId)),
      parentSpanContext(parentSpanContext.map([](const InvocationSpanContext& ctx) {
        return kj::heap<InvocationSpanContext>(ctx.clone());
      })) {}

InvocationSpanContext InvocationSpanContext::newChild() const {
  KJ_ASSERT(!isTrigger(), "unable to create child spans on this context");
  kj::Maybe<kj::EntropySource&> otherEntropySource = entropySource.map(
      [](auto& es) -> kj::EntropySource& { return const_cast<kj::EntropySource&>(es); });
  return InvocationSpanContext(kj::Badge<InvocationSpanContext>(), otherEntropySource, traceId,
      invocationId, SpanId::fromEntropy(otherEntropySource), *this);
}

InvocationSpanContext InvocationSpanContext::newForInvocation(
    kj::Maybe<const InvocationSpanContext&> triggerContext,
    kj::Maybe<kj::EntropySource&> entropySource) {
  kj::Maybe<const InvocationSpanContext&> parent;
  auto traceId = triggerContext
                     .map([&](auto& ctx) mutable {
    parent = ctx;
    return ctx.traceId;
  }).orDefault([&] { return TraceId::fromEntropy(entropySource); });
  return InvocationSpanContext(kj::Badge<InvocationSpanContext>(), entropySource, kj::mv(traceId),
      TraceId::fromEntropy(entropySource), SpanId::fromEntropy(entropySource), kj::mv(parent));
}

TraceId TraceId::fromCapnp(rpc::TraceId::Reader reader) {
  return TraceId(reader.getLow(), reader.getHigh());
}

void TraceId::toCapnp(rpc::TraceId::Builder writer) const {
  writer.setLow(low);
  writer.setHigh(high);
}

kj::Maybe<InvocationSpanContext> InvocationSpanContext::fromCapnp(
    rpc::InvocationSpanContext::Reader reader) {
  if (!reader.hasTraceId() || !reader.hasInvocationId()) {
    // If the reader does not have a traceId or invocationId field then it is
    // invalid and we will just ignore it.
    return kj::none;
  }

  auto sc = InvocationSpanContext(kj::Badge<InvocationSpanContext>(), kj::none,
      TraceId::fromCapnp(reader.getTraceId()), TraceId::fromCapnp(reader.getInvocationId()),
      reader.getSpanId());
  // If the traceId or invocationId are invalid, then we'll ignore them.
  if (!sc.getTraceId() || !sc.getInvocationId()) return kj::none;
  return kj::mv(sc);
}

void InvocationSpanContext::toCapnp(rpc::InvocationSpanContext::Builder writer) const {
  traceId.toCapnp(writer.initTraceId());
  invocationId.toCapnp(writer.initInvocationId());
  writer.setSpanId(spanId);
}

InvocationSpanContext InvocationSpanContext::clone() const {
  kj::Maybe<kj::EntropySource&> otherEntropySource = entropySource.map(
      [](auto& es) -> kj::EntropySource& { return const_cast<kj::EntropySource&>(es); });
  return InvocationSpanContext(kj::Badge<InvocationSpanContext>(), otherEntropySource, traceId,
      invocationId, spanId,
      parentSpanContext.map([](auto& ctx) -> const InvocationSpanContext& { return *ctx.get(); }));
}

kj::String KJ_STRINGIFY(const InvocationSpanContext& context) {
  return kj::str(context.getTraceId(), "-", context.getInvocationId(), "-", context.getSpanId());
}

kj::String KJ_STRINGIFY(const TailEvent::Event& event) {
  KJ_SWITCH_ONEOF(event) {
    KJ_CASE_ONEOF(onset, Onset) {
      return kj::str("Onset");
    }
    KJ_CASE_ONEOF(outcome, Outcome) {
      return kj::str("Outcome");
    }
    KJ_CASE_ONEOF(spanOpen, SpanOpen) {
      return spanOpen.toString();
    }
    KJ_CASE_ONEOF(spanClose, SpanClose) {
      return spanClose.toString();
    }
    KJ_CASE_ONEOF(diagnosticChannelEvent, DiagnosticChannelEvent) {
      return kj::str("diagnosticChannelEvent");
    }
    KJ_CASE_ONEOF(exception, Exception) {
      return kj::str("Exception");
    }
    KJ_CASE_ONEOF(log, Log) {
      return kj::str("Log");
    }
    KJ_CASE_ONEOF(ret, Return) {
      return kj::str("Return");
    }
    KJ_CASE_ONEOF(customInfo, CustomInfo) {
      return kj::str(customInfo);
    }
  }
  KJ_UNREACHABLE
}

kj::String KJ_STRINGIFY(const CustomInfo& customInfo) {
  return kj::str(
      "CustomInfo: ", kj::strArray(KJ_MAP(attr, customInfo) { return kj::str(attr); }, ", "));
}

SpanContext SpanContext::fromCapnp(rpc::SpanContext::Reader reader) {
  auto info = reader.getInfo();
  kj::Maybe<SpanId> spanId;
  if (info.isSpanId()) {
    spanId = info.getSpanId();
  }

  return SpanContext(TraceId::fromCapnp(reader.getTraceId()), spanId);
}

void SpanContext::toCapnp(rpc::SpanContext::Builder writer) const {
  traceId.toCapnp(writer.initTraceId());
  auto info = writer.initInfo();
  KJ_IF_SOME(s, spanId) {
    info.setSpanId(s);
  }
}

SpanContext SpanContext::clone() const {
  return SpanContext(traceId, spanId);
}

kj::String KJ_STRINGIFY(const SpanContext& context) {
  return kj::str(context.getTraceId(), "-", context.getSpanId());
}

namespace {

static kj::HttpMethod validateMethod(capnp::HttpMethod method) {
  KJ_REQUIRE(method <= capnp::HttpMethod::BAN, "unknown method", method);
  return static_cast<kj::HttpMethod>(method);
}

}  // namespace

FetchEventInfo::FetchEventInfo(
    kj::HttpMethod method, kj::String url, kj::String cfJson, kj::Array<Header> headers)
    : method(method),
      url(kj::mv(url)),
      cfJson(kj::mv(cfJson)),
      headers(kj::mv(headers)) {}

FetchEventInfo::FetchEventInfo(rpc::Trace::FetchEventInfo::Reader reader)
    : method(validateMethod(reader.getMethod())),
      url(kj::str(reader.getUrl())),
      cfJson(kj::str(reader.getCfJson())) {
  // Note: Request body size is now tracked in FetchResponseInfo, not here.
  // The obsolete4/obsolete5 fields in the schema are ignored.
  kj::Vector<Header> v;
  v.addAll(reader.getHeaders());
  headers = v.releaseAsArray();
}

void FetchEventInfo::copyTo(rpc::Trace::FetchEventInfo::Builder builder) const {
  builder.setMethod(static_cast<capnp::HttpMethod>(method));
  builder.setUrl(url);
  builder.setCfJson(cfJson);
  // Note: Request body size is now tracked in FetchResponseInfo, not here.

  auto list = builder.initHeaders(headers.size());
  for (auto i: kj::indices(headers)) {
    headers[i].copyTo(list[i]);
  }
}

FetchEventInfo FetchEventInfo::clone() const {
  return FetchEventInfo(
      method, kj::str(url), kj::str(cfJson), KJ_MAP(h, headers) { return h.clone(); });
}

kj::String FetchEventInfo::toString() const {
  return kj::str("FetchEventInfo: ",
      kj::delimited(
          kj::arr(kj::str(method), kj::str(url), kj::str(cfJson), kj::str(headers)), ", "_kjc));
}

FetchEventInfo::Header::Header(kj::String name, kj::String value)
    : name(kj::mv(name)),
      value(kj::mv(value)) {}

FetchEventInfo::Header::Header(rpc::Trace::FetchEventInfo::Header::Reader reader)
    : name(kj::str(reader.getName())),
      value(kj::str(reader.getValue())) {}

void FetchEventInfo::Header::copyTo(rpc::Trace::FetchEventInfo::Header::Builder builder) const {
  builder.setName(name);
  builder.setValue(value);
}

FetchEventInfo::Header FetchEventInfo::Header::clone() const {
  return Header(kj::str(name), kj::str(value));
}

kj::String FetchEventInfo::Header::toString() const {
  return kj::str("FetchEventInfo::Header: ", name, ", ", value);
}

JsRpcEventInfo::JsRpcEventInfo(kj::String methodName): methodName(kj::mv(methodName)) {}

JsRpcEventInfo::JsRpcEventInfo(rpc::Trace::JsRpcEventInfo::Reader reader)
    : methodName(kj::str(reader.getMethodName())) {}

void JsRpcEventInfo::copyTo(rpc::Trace::JsRpcEventInfo::Builder builder) const {
  builder.setMethodName(methodName);
}

JsRpcEventInfo JsRpcEventInfo::clone() const {
  return JsRpcEventInfo(kj::str(methodName));
}

kj::String JsRpcEventInfo::toString() const {
  return kj::str("JsRpcEventInfo: ", methodName);
}

ScheduledEventInfo::ScheduledEventInfo(double scheduledTime, kj::String cron)
    : scheduledTime(scheduledTime),
      cron(kj::mv(cron)) {}

ScheduledEventInfo::ScheduledEventInfo(rpc::Trace::ScheduledEventInfo::Reader reader)
    : scheduledTime(reader.getScheduledTime()),
      cron(kj::str(reader.getCron())) {}

void ScheduledEventInfo::copyTo(rpc::Trace::ScheduledEventInfo::Builder builder) const {
  builder.setScheduledTime(scheduledTime);
  builder.setCron(cron);
}

ScheduledEventInfo ScheduledEventInfo::clone() const {
  return ScheduledEventInfo(scheduledTime, kj::str(cron));
}

AlarmEventInfo::AlarmEventInfo(kj::Date scheduledTime): scheduledTime(scheduledTime) {}

AlarmEventInfo::AlarmEventInfo(rpc::Trace::AlarmEventInfo::Reader reader)
    : scheduledTime(reader.getScheduledTimeMs() * kj::MILLISECONDS + kj::UNIX_EPOCH) {}

void AlarmEventInfo::copyTo(rpc::Trace::AlarmEventInfo::Builder builder) const {
  builder.setScheduledTimeMs((scheduledTime - kj::UNIX_EPOCH) / kj::MILLISECONDS);
}

AlarmEventInfo AlarmEventInfo::clone() const {
  return AlarmEventInfo(scheduledTime);
}

QueueEventInfo::QueueEventInfo(kj::String queueName, uint32_t batchSize)
    : queueName(kj::mv(queueName)),
      batchSize(batchSize) {}

QueueEventInfo::QueueEventInfo(rpc::Trace::QueueEventInfo::Reader reader)
    : queueName(kj::heapString(reader.getQueueName())),
      batchSize(reader.getBatchSize()) {}

void QueueEventInfo::copyTo(rpc::Trace::QueueEventInfo::Builder builder) const {
  builder.setQueueName(queueName);
  builder.setBatchSize(batchSize);
}

QueueEventInfo QueueEventInfo::clone() const {
  return QueueEventInfo(kj::str(queueName), batchSize);
}

EmailEventInfo::EmailEventInfo(kj::String mailFrom, kj::String rcptTo, uint32_t rawSize)
    : mailFrom(kj::mv(mailFrom)),
      rcptTo(kj::mv(rcptTo)),
      rawSize(rawSize) {}

EmailEventInfo::EmailEventInfo(rpc::Trace::EmailEventInfo::Reader reader)
    : mailFrom(kj::heapString(reader.getMailFrom())),
      rcptTo(kj::heapString(reader.getRcptTo())),
      rawSize(reader.getRawSize()) {}

void EmailEventInfo::copyTo(rpc::Trace::EmailEventInfo::Builder builder) const {
  builder.setMailFrom(mailFrom);
  builder.setRcptTo(rcptTo);
  builder.setRawSize(rawSize);
}

EmailEventInfo EmailEventInfo::clone() const {
  return EmailEventInfo(kj::str(mailFrom), kj::str(rcptTo), rawSize);
}

namespace {
kj::Vector<TraceEventInfo::TraceItem> getTraceItemsFromTraces(
    kj::ArrayPtr<const kj::Own<Trace>> traces) {
  return KJ_MAP(t, traces) { return TraceEventInfo::TraceItem(mapCopyString(t->scriptName)); };
}

kj::Vector<TraceEventInfo::TraceItem> getTraceItemsFromReader(
    rpc::Trace::TraceEventInfo::Reader reader) {
  return KJ_MAP(r, reader.getTraces()) { return TraceEventInfo::TraceItem(r); };
}
}  // namespace

TraceEventInfo::TraceEventInfo(kj::ArrayPtr<const kj::Own<Trace>> traces)
    : traces(getTraceItemsFromTraces(traces)) {}

TraceEventInfo::TraceEventInfo(rpc::Trace::TraceEventInfo::Reader reader)
    : traces(getTraceItemsFromReader(reader)) {}

void TraceEventInfo::copyTo(rpc::Trace::TraceEventInfo::Builder builder) const {
  auto list = builder.initTraces(traces.size());
  for (auto i: kj::indices(traces)) {
    traces[i].copyTo(list[i]);
  }
}

TraceEventInfo TraceEventInfo::clone() const {
  return TraceEventInfo(KJ_MAP(item, traces) { return item.clone(); });
}

TraceEventInfo::TraceItem::TraceItem(kj::Maybe<kj::String> scriptName)
    : scriptName(kj::mv(scriptName)) {}

TraceEventInfo::TraceItem::TraceItem(rpc::Trace::TraceEventInfo::TraceItem::Reader reader)
    : scriptName(kj::str(reader.getScriptName())) {}

void TraceEventInfo::TraceItem::copyTo(
    rpc::Trace::TraceEventInfo::TraceItem::Builder builder) const {
  KJ_IF_SOME(name, scriptName) {
    builder.setScriptName(name);
  }
}

TraceEventInfo::TraceItem TraceEventInfo::TraceItem::clone() const {
  return TraceItem(mapCopyString(scriptName));
}

DiagnosticChannelEvent::DiagnosticChannelEvent(
    kj::Date timestamp, kj::String channel, kj::Array<kj::byte> message)
    : timestamp(timestamp),
      channel(kj::mv(channel)),
      message(kj::mv(message)) {}

DiagnosticChannelEvent::DiagnosticChannelEvent(rpc::Trace::DiagnosticChannelEvent::Reader reader)
    : timestamp(kj::UNIX_EPOCH + reader.getTimestampNs() * kj::NANOSECONDS),
      channel(kj::heapString(reader.getChannel())),
      message(kj::heapArray<kj::byte>(reader.getMessage())) {}

void DiagnosticChannelEvent::copyTo(rpc::Trace::DiagnosticChannelEvent::Builder builder) const {
  builder.setTimestampNs((timestamp - kj::UNIX_EPOCH) / kj::NANOSECONDS);
  builder.setChannel(channel);
  builder.setMessage(message);
}

DiagnosticChannelEvent DiagnosticChannelEvent::clone() const {
  return DiagnosticChannelEvent(timestamp, kj::str(channel), kj::heapArray<kj::byte>(message));
}

HibernatableWebSocketEventInfo::HibernatableWebSocketEventInfo(Type type): type(type) {}

HibernatableWebSocketEventInfo::HibernatableWebSocketEventInfo(
    rpc::Trace::HibernatableWebSocketEventInfo::Reader reader)
    : type(readFrom(reader)) {}

void HibernatableWebSocketEventInfo::copyTo(
    rpc::Trace::HibernatableWebSocketEventInfo::Builder builder) const {
  auto typeBuilder = builder.initType();
  KJ_SWITCH_ONEOF(type) {
    KJ_CASE_ONEOF(_, Message) {
      typeBuilder.setMessage();
    }
    KJ_CASE_ONEOF(close, Close) {
      auto closeBuilder = typeBuilder.initClose();
      closeBuilder.setCode(close.code);
      closeBuilder.setWasClean(close.wasClean);
    }
    KJ_CASE_ONEOF(_, Error) {
      typeBuilder.setError();
    }
  }
}

HibernatableWebSocketEventInfo HibernatableWebSocketEventInfo::clone() const {
  KJ_SWITCH_ONEOF(type) {
    KJ_CASE_ONEOF(_, Message) {
      return HibernatableWebSocketEventInfo(Message{});
    }
    KJ_CASE_ONEOF(_, Error) {
      return HibernatableWebSocketEventInfo(Error{});
    }
    KJ_CASE_ONEOF(close, Close) {
      return HibernatableWebSocketEventInfo(Close{
        .code = close.code,
        .wasClean = close.wasClean,
      });
    }
  }
  KJ_UNREACHABLE;
}

HibernatableWebSocketEventInfo::Type HibernatableWebSocketEventInfo::readFrom(
    rpc::Trace::HibernatableWebSocketEventInfo::Reader reader) {
  auto type = reader.getType();
  switch (type.which()) {
    case rpc::Trace::HibernatableWebSocketEventInfo::Type::MESSAGE: {
      return Message{};
    }
    case rpc::Trace::HibernatableWebSocketEventInfo::Type::CLOSE: {
      auto close = type.getClose();
      return Close{
        .code = close.getCode(),
        .wasClean = close.getWasClean(),
      };
    }
    case rpc::Trace::HibernatableWebSocketEventInfo::Type::ERROR: {
      return Error{};
    }
  }
}

FetchResponseInfo::FetchResponseInfo(
    uint16_t statusCode, kj::Maybe<uint64_t> bodySize, kj::Maybe<uint64_t> requestBodySize)
    : statusCode(statusCode),
      bodySize(bodySize),
      requestBodySize(requestBodySize) {}

FetchResponseInfo::FetchResponseInfo(rpc::Trace::FetchResponseInfo::Reader reader)
    : statusCode(reader.getStatusCode()) {
  // Only set bodySize if hasBodySize is true
  if (reader.getHasBodySize()) {
    bodySize = reader.getBodySize();
  }
  // Only set requestBodySize if hasRequestBodySize is true
  if (reader.getHasRequestBodySize()) {
    requestBodySize = reader.getRequestBodySize();
  }
}

void FetchResponseInfo::copyTo(rpc::Trace::FetchResponseInfo::Builder builder) const {
  builder.setStatusCode(statusCode);
  KJ_IF_SOME(size, bodySize) {
    builder.setBodySize(size);
    builder.setHasBodySize(true);
  }
  KJ_IF_SOME(size, requestBodySize) {
    builder.setRequestBodySize(size);
    builder.setHasRequestBodySize(true);
  }
}

FetchResponseInfo FetchResponseInfo::clone() const {
  return FetchResponseInfo(statusCode, bodySize, requestBodySize);
}

Log::Log(kj::Date timestamp, LogLevel logLevel, kj::String message)
    : timestamp(timestamp),
      logLevel(logLevel),
      message(kj::mv(message)) {}

void Log::copyTo(rpc::Trace::Log::Builder builder) const {
  builder.setTimestampNs((timestamp - kj::UNIX_EPOCH) / kj::NANOSECONDS);
  builder.setLogLevel(logLevel);
  builder.setMessage(message);
}

Log Log::clone() const {
  return Log(timestamp, logLevel, kj::str(message));
}

Exception::Exception(
    kj::Date timestamp, kj::String name, kj::String message, kj::Maybe<kj::String> stack)
    : timestamp(timestamp),
      name(kj::mv(name)),
      message(kj::mv(message)),
      stack(kj::mv(stack)) {}

Log::Log(rpc::Trace::Log::Reader reader)
    : timestamp(kj::UNIX_EPOCH + reader.getTimestampNs() * kj::NANOSECONDS),
      logLevel(reader.getLogLevel()),
      message(kj::str(reader.getMessage())) {}

Exception::Exception(rpc::Trace::Exception::Reader reader)
    : timestamp(kj::UNIX_EPOCH + reader.getTimestampNs() * kj::NANOSECONDS),
      name(kj::str(reader.getName())),
      message(kj::str(reader.getMessage())) {
  if (reader.hasStack()) {
    stack = kj::str(reader.getStack());
  }
}

void Exception::copyTo(rpc::Trace::Exception::Builder builder) const {
  builder.setTimestampNs((timestamp - kj::UNIX_EPOCH) / kj::NANOSECONDS);
  builder.setName(name);
  builder.setMessage(message);
  KJ_IF_SOME(s, stack) {
    builder.setStack(s);
  }
}

Exception Exception::clone() const {
  return Exception(timestamp, kj::str(name), kj::str(message), mapCopyString(stack));
}
}  // namespace tracing

Trace::Trace(kj::Maybe<kj::String> stableId,
    kj::Maybe<kj::String> scriptName,
    kj::Maybe<kj::Own<ScriptVersion::Reader>> scriptVersion,
    kj::Maybe<kj::String> dispatchNamespace,
    kj::Maybe<kj::String> scriptId,
    kj::Array<kj::String> scriptTags,
    kj::Maybe<kj::String> entrypoint,
    ExecutionModel executionModel,
    kj::Maybe<kj::String> durableObjectId)
    : stableId(kj::mv(stableId)),
      scriptName(kj::mv(scriptName)),
      scriptVersion(kj::mv(scriptVersion)),
      dispatchNamespace(kj::mv(dispatchNamespace)),
      scriptId(kj::mv(scriptId)),
      scriptTags(kj::mv(scriptTags)),
      entrypoint(kj::mv(entrypoint)),
      durableObjectId(kj::mv(durableObjectId)),
      executionModel(executionModel) {}
Trace::Trace(rpc::Trace::Reader reader) {
  mergeFrom(reader, PipelineLogLevel::FULL);
}

Trace::~Trace() noexcept(false) {}

void Trace::copyTo(rpc::Trace::Builder builder) const {
  {
    auto list = builder.initLogs(logs.size());
    for (auto i: kj::indices(logs)) {
      logs[i].copyTo(list[i]);
    }
  }

  {
    auto list = builder.initExceptions(exceptions.size());
    for (auto i: kj::indices(exceptions)) {
      exceptions[i].copyTo(list[i]);
    }
  }

  builder.setTruncated(truncated);
  builder.setOutcome(outcome);
  builder.setCpuTime(cpuTime / kj::MILLISECONDS);
  builder.setWallTime(wallTime / kj::MILLISECONDS);
  KJ_IF_SOME(name, scriptName) {
    builder.setScriptName(name);
  }
  KJ_IF_SOME(version, scriptVersion) {
    builder.setScriptVersion(*version);
  }
  KJ_IF_SOME(id, scriptId) {
    builder.setScriptId(id);
  }
  KJ_IF_SOME(ns, dispatchNamespace) {
    builder.setDispatchNamespace(ns);
  }
  builder.setExecutionModel(executionModel);

  {
    auto list = builder.initScriptTags(scriptTags.size());
    for (auto i: kj::indices(scriptTags)) {
      list.set(i, scriptTags[i]);
    }
  }

  KJ_IF_SOME(e, entrypoint) {
    builder.setEntrypoint(e);
  }

  KJ_IF_SOME(id, durableObjectId) {
    builder.setDurableObjectId(id);
  }

  builder.setEventTimestampNs((eventTimestamp - kj::UNIX_EPOCH) / kj::NANOSECONDS);

  auto eventInfoBuilder = builder.initEventInfo();
  KJ_IF_SOME(e, eventInfo) {
    KJ_SWITCH_ONEOF(e) {
      KJ_CASE_ONEOF(fetch, tracing::FetchEventInfo) {
        auto fetchBuilder = eventInfoBuilder.initFetch();
        fetch.copyTo(fetchBuilder);
      }
      KJ_CASE_ONEOF(jsRpc, tracing::JsRpcEventInfo) {
        auto jsRpcBuilder = eventInfoBuilder.initJsRpc();
        jsRpc.copyTo(jsRpcBuilder);
      }
      KJ_CASE_ONEOF(scheduled, tracing::ScheduledEventInfo) {
        auto scheduledBuilder = eventInfoBuilder.initScheduled();
        scheduled.copyTo(scheduledBuilder);
      }
      KJ_CASE_ONEOF(alarm, tracing::AlarmEventInfo) {
        auto alarmBuilder = eventInfoBuilder.initAlarm();
        alarm.copyTo(alarmBuilder);
      }
      KJ_CASE_ONEOF(queue, tracing::QueueEventInfo) {
        auto queueBuilder = eventInfoBuilder.initQueue();
        queue.copyTo(queueBuilder);
      }
      KJ_CASE_ONEOF(email, tracing::EmailEventInfo) {
        auto emailBuilder = eventInfoBuilder.initEmail();
        email.copyTo(emailBuilder);
      }
      KJ_CASE_ONEOF(trace, tracing::TraceEventInfo) {
        auto traceBuilder = eventInfoBuilder.initTrace();
        trace.copyTo(traceBuilder);
      }
      KJ_CASE_ONEOF(hibWs, tracing::HibernatableWebSocketEventInfo) {
        auto hibWsBuilder = eventInfoBuilder.initHibernatableWebSocket();
        hibWs.copyTo(hibWsBuilder);
      }
      KJ_CASE_ONEOF(custom, tracing::CustomEventInfo) {
        eventInfoBuilder.initCustom();
      }
    }
  } else {
    eventInfoBuilder.setNone();
  }

  KJ_IF_SOME(fetchResponseInfo, this->fetchResponseInfo) {
    auto fetchResponseInfoBuilder = builder.initResponse();
    fetchResponseInfo.copyTo(fetchResponseInfoBuilder);
  }

  {
    auto list = builder.initDiagnosticChannelEvents(diagnosticChannelEvents.size());
    for (auto i: kj::indices(diagnosticChannelEvents)) {
      diagnosticChannelEvents[i].copyTo(list[i]);
    }
  }
}

void Trace::mergeFrom(rpc::Trace::Reader reader, PipelineLogLevel pipelineLogLevel) {
  // Sandboxed workers currently record their traces as if the pipeline log level were set to
  // "full", so we may need to filter out the extra data after receiving the traces back.
  if (pipelineLogLevel != PipelineLogLevel::NONE) {
    logs.addAll(reader.getLogs());
    exceptions.addAll(reader.getExceptions());
    diagnosticChannelEvents.addAll(reader.getDiagnosticChannelEvents());
  }

  truncated = reader.getTruncated();
  outcome = reader.getOutcome();
  cpuTime = reader.getCpuTime() * kj::MILLISECONDS;
  wallTime = reader.getWallTime() * kj::MILLISECONDS;

  // mergeFrom() is called both when deserializing traces from a sandboxed
  // worker and when deserializing traces sent to a sandboxed trace worker. In
  // the former case, the trace's scriptName (and other fields like
  // scriptVersion) are already set and the deserialized value is missing, so
  // we need to be careful not to overwrite the set value.
  if (reader.hasScriptName()) {
    scriptName = kj::str(reader.getScriptName());
  }

  if (reader.hasScriptVersion()) {
    scriptVersion = capnp::clone(reader.getScriptVersion());
  }

  if (reader.hasScriptId()) {
    scriptId = kj::str(reader.getScriptId());
  }

  if (reader.hasDispatchNamespace()) {
    dispatchNamespace = kj::str(reader.getDispatchNamespace());
  }
  executionModel = reader.getExecutionModel();

  if (auto tags = reader.getScriptTags(); tags.size() > 0) {
    scriptTags = KJ_MAP(tag, tags) { return kj::str(tag); };
  }

  if (reader.hasEntrypoint()) {
    entrypoint = kj::str(reader.getEntrypoint());
  }

  if (reader.hasDurableObjectId()) {
    durableObjectId = kj::str(reader.getDurableObjectId());
  }

  eventTimestamp = kj::UNIX_EPOCH + reader.getEventTimestampNs() * kj::NANOSECONDS;

  if (pipelineLogLevel == PipelineLogLevel::NONE) {
    eventInfo = kj::none;
  } else {
    auto e = reader.getEventInfo();
    switch (e.which()) {
      case rpc::Trace::EventInfo::Which::FETCH:
        eventInfo = tracing::FetchEventInfo(e.getFetch());
        break;
      case rpc::Trace::EventInfo::Which::JS_RPC:
        eventInfo = tracing::JsRpcEventInfo(e.getJsRpc());
        break;
      case rpc::Trace::EventInfo::Which::SCHEDULED:
        eventInfo = tracing::ScheduledEventInfo(e.getScheduled());
        break;
      case rpc::Trace::EventInfo::Which::ALARM:
        eventInfo = tracing::AlarmEventInfo(e.getAlarm());
        break;
      case rpc::Trace::EventInfo::Which::QUEUE:
        eventInfo = tracing::QueueEventInfo(e.getQueue());
        break;
      case rpc::Trace::EventInfo::Which::EMAIL:
        eventInfo = tracing::EmailEventInfo(e.getEmail());
        break;
      case rpc::Trace::EventInfo::Which::TRACE:
        eventInfo = tracing::TraceEventInfo(e.getTrace());
        break;
      case rpc::Trace::EventInfo::Which::HIBERNATABLE_WEB_SOCKET:
        eventInfo = tracing::HibernatableWebSocketEventInfo(e.getHibernatableWebSocket());
        break;
      case rpc::Trace::EventInfo::Which::CUSTOM:
        eventInfo = tracing::CustomEventInfo(e.getCustom());
        break;
      case rpc::Trace::EventInfo::Which::NONE:
        eventInfo = kj::none;
        break;
    }
  }

  if (reader.hasResponse()) {
    fetchResponseInfo = tracing::FetchResponseInfo(reader.getResponse());
  }
}

namespace tracing {

Attribute::Attribute(kj::ConstString name, Value&& value)
    : name(kj::mv(name)),
      value(kj::arr(kj::mv(value))) {}

Attribute::Attribute(kj::ConstString name, Values&& value)
    : name(kj::mv(name)),
      value(kj::mv(value)) {}

namespace {
kj::Array<Attribute::Value> readValues(const rpc::Trace::Attribute::Reader& reader) {
  // There should always be a value and it always have at least one entry in the list.
  KJ_ASSERT(reader.hasValue());
  auto value = reader.getValue();
  return KJ_MAP(v, value) { return deserializeTagValue(v); };
}

kj::Maybe<FetchResponseInfo> readReturnInfo(const rpc::Trace::Return::Reader& reader) {
  auto info = reader.getInfo();
  switch (info.which()) {
    case rpc::Trace::Return::Info::EMPTY:
      return kj::none;
    case rpc::Trace::Return::Info::FETCH: {
      return kj::Maybe(FetchResponseInfo(info.getFetch()));
    }
  }
  KJ_UNREACHABLE;
}
}  // namespace

Attribute::Attribute(rpc::Trace::Attribute::Reader reader)
    : name(kj::str(reader.getName())),
      value(readValues(reader)) {}

void Attribute::copyTo(rpc::Trace::Attribute::Builder builder) const {
  builder.setName(name.asPtr());
  auto vec = builder.initValue(value.size());
  for (size_t n = 0; n < value.size(); n++) {
    serializeTagValue(vec[n], value[n]);
  }
}

Attribute Attribute::clone() const {
  return Attribute(name.clone(), KJ_MAP(v, value) { return spanTagClone(v); });
}

kj::String Attribute::toString() const {
  return kj::str("Attribute: ", name, ", ", value);
}

Return::Return(kj::Maybe<FetchResponseInfo> info): info(kj::mv(info)) {}
Return::Return(rpc::Trace::Return::Reader reader): info(readReturnInfo(reader)) {}

void Return::copyTo(rpc::Trace::Return::Builder builder) const {
  KJ_IF_SOME(fetchInfo, info) {
    auto infoBuilder = builder.initInfo();
    fetchInfo.copyTo(infoBuilder.initFetch());
  }
}

Return Return::clone() const {
  KJ_IF_SOME(fetchInfo, info) {
    return Return(kj::Maybe(fetchInfo.clone()));
  }
  return Return();
}

SpanOpen::SpanOpen(SpanId spanId, kj::ConstString operationName, kj::Maybe<Info> info)
    : operationName(kj::mv(operationName)),
      info(kj::mv(info)),
      spanId(spanId) {}

namespace {
kj::Maybe<SpanOpen::Info> readSpanOpenInfo(rpc::Trace::SpanOpen::Reader& reader) {
  auto info = reader.getInfo();
  switch (info.which()) {
    case rpc::Trace::SpanOpen::Info::EMPTY:
      return kj::none;
    case rpc::Trace::SpanOpen::Info::FETCH: {
      return kj::Maybe(FetchEventInfo(info.getFetch()));
    }
    case rpc::Trace::SpanOpen::Info::JS_RPC: {
      return kj::Maybe(JsRpcEventInfo(info.getJsRpc()));
    }
    case rpc::Trace::SpanOpen::Info::CUSTOM: {
      auto custom = info.getCustom();
      return kj::Maybe(KJ_MAP(a, custom) { return Attribute(a); });
    }
  }
  KJ_UNREACHABLE;
}
}  // namespace

SpanOpen::SpanOpen(rpc::Trace::SpanOpen::Reader reader)
    : operationName(kj::str(reader.getOperationName())),
      info(readSpanOpenInfo(reader)),
      spanId(reader.getSpanId()) {}

void SpanOpen::copyTo(rpc::Trace::SpanOpen::Builder builder) const {
  builder.setOperationName(operationName.asPtr());
  builder.setSpanId(spanId);
  KJ_IF_SOME(i, info) {
    auto infoBuilder = builder.initInfo();
    KJ_SWITCH_ONEOF(i) {
      KJ_CASE_ONEOF(fetch, FetchEventInfo) {
        fetch.copyTo(infoBuilder.initFetch());
      }
      KJ_CASE_ONEOF(jsrpc, JsRpcEventInfo) {
        jsrpc.copyTo(infoBuilder.initJsRpc());
      }
      KJ_CASE_ONEOF(custom, CustomInfo) {
        auto customBuilder = infoBuilder.initCustom(custom.size());
        for (size_t n = 0; n < custom.size(); n++) {
          custom[n].copyTo(customBuilder[n]);
        }
      }
    }
  }
}

SpanOpen SpanOpen::clone() const {
  constexpr auto cloneInfo = [](const kj::Maybe<Info>& info) -> kj::Maybe<SpanOpen::Info> {
    return info.map([](const Info& info) -> SpanOpen::Info {
      KJ_SWITCH_ONEOF(info) {
        KJ_CASE_ONEOF(fetch, FetchEventInfo) {
          return fetch.clone();
        }
        KJ_CASE_ONEOF(jsrpc, JsRpcEventInfo) {
          return jsrpc.clone();
        }
        KJ_CASE_ONEOF(custom, CustomInfo) {
          return KJ_MAP(attr, custom) { return attr.clone(); };
        }
      }
      KJ_UNREACHABLE;
    });
  };
  return SpanOpen(spanId, operationName.clone(), cloneInfo(info));
}

kj::String KJ_STRINGIFY(const SpanOpen::Info& info) {
  KJ_SWITCH_ONEOF(info) {
    KJ_CASE_ONEOF(fetch, FetchEventInfo) {
      return fetch.toString();
    }
    KJ_CASE_ONEOF(jsrpc, JsRpcEventInfo) {
      return jsrpc.toString();
    }
    KJ_CASE_ONEOF(customInfo, CustomInfo) {
      return kj::str(customInfo);
    }
  }
  KJ_UNREACHABLE
}

kj::String SpanOpen::toString() const {
  return kj::str("SpanOpen:", operationName, ", ", info);
}

SpanClose::SpanClose(EventOutcome outcome): outcome(outcome) {}

SpanClose::SpanClose(rpc::Trace::SpanClose::Reader reader): outcome(reader.getOutcome()) {}

void SpanClose::copyTo(rpc::Trace::SpanClose::Builder builder) const {
  builder.setOutcome(outcome);
}

SpanClose SpanClose::clone() const {
  return SpanClose(outcome);
}

kj::String SpanClose::toString() const {
  return kj::str("SpanClose: ", outcome);
}

Onset::Info readOnsetInfo(const rpc::Trace::Onset::Info::Reader& info) {
  switch (info.which()) {
    case rpc::Trace::Onset::Info::FETCH: {
      return FetchEventInfo(info.getFetch());
    }
    case rpc::Trace::Onset::Info::JS_RPC: {
      return JsRpcEventInfo(info.getJsRpc());
    }
    case rpc::Trace::Onset::Info::SCHEDULED: {
      return ScheduledEventInfo(info.getScheduled());
    }
    case rpc::Trace::Onset::Info::ALARM: {
      return AlarmEventInfo(info.getAlarm());
    }
    case rpc::Trace::Onset::Info::QUEUE: {
      return QueueEventInfo(info.getQueue());
    }
    case rpc::Trace::Onset::Info::EMAIL: {
      return EmailEventInfo(info.getEmail());
    }
    case rpc::Trace::Onset::Info::TRACE: {
      return TraceEventInfo(info.getTrace());
    }
    case rpc::Trace::Onset::Info::HIBERNATABLE_WEB_SOCKET: {
      return HibernatableWebSocketEventInfo(info.getHibernatableWebSocket());
    }
    case rpc::Trace::Onset::Info::CUSTOM: {
      return CustomEventInfo();
    }
  }
  KJ_UNREACHABLE;
}

void writeOnsetInfo(const Onset::Info& info, rpc::Trace::Onset::Info::Builder& infoBuilder) {
  KJ_SWITCH_ONEOF(info) {
    KJ_CASE_ONEOF(fetch, FetchEventInfo) {
      fetch.copyTo(infoBuilder.initFetch());
    }
    KJ_CASE_ONEOF(jsrpc, JsRpcEventInfo) {
      jsrpc.copyTo(infoBuilder.initJsRpc());
    }
    KJ_CASE_ONEOF(scheduled, ScheduledEventInfo) {
      scheduled.copyTo(infoBuilder.initScheduled());
    }
    KJ_CASE_ONEOF(alarm, AlarmEventInfo) {
      alarm.copyTo(infoBuilder.initAlarm());
    }
    KJ_CASE_ONEOF(queue, QueueEventInfo) {
      queue.copyTo(infoBuilder.initQueue());
    }
    KJ_CASE_ONEOF(email, EmailEventInfo) {
      email.copyTo(infoBuilder.initEmail());
    }
    KJ_CASE_ONEOF(trace, TraceEventInfo) {
      trace.copyTo(infoBuilder.initTrace());
    }
    KJ_CASE_ONEOF(hws, HibernatableWebSocketEventInfo) {
      hws.copyTo(infoBuilder.initHibernatableWebSocket());
    }
    KJ_CASE_ONEOF(custom, CustomEventInfo) {
      infoBuilder.initCustom();
    }
  }
}

namespace {
kj::Maybe<kj::String> getScriptNameFromReader(const rpc::Trace::Onset::Reader& reader) {
  if (reader.hasScriptName()) {
    return kj::str(reader.getScriptName());
  }
  return kj::none;
}

kj::Maybe<kj::Own<ScriptVersion::Reader>> getScriptVersionFromReader(
    const rpc::Trace::Onset::Reader& reader) {
  if (reader.hasScriptVersion()) {
    return capnp::clone(reader.getScriptVersion());
  }
  return kj::none;
}

kj::Maybe<kj::String> getDispatchNamespaceFromReader(const rpc::Trace::Onset::Reader& reader) {
  if (reader.hasDispatchNamespace()) {
    return kj::str(reader.getDispatchNamespace());
  }
  return kj::none;
}

kj::Maybe<kj::String> getScriptIdFromReader(const rpc::Trace::Onset::Reader& reader) {
  if (reader.hasScriptId()) {
    return kj::str(reader.getScriptId());
  }
  return kj::none;
}

kj::Maybe<kj::Array<kj::String>> getScriptTagsFromReader(const rpc::Trace::Onset::Reader& reader) {
  if (reader.hasScriptTags()) {
    auto tags = reader.getScriptTags();
    kj::Vector<kj::String> scriptTags(tags.size());
    for (size_t i = 0; i < tags.size(); i++) {
      scriptTags.add(kj::str(tags[i]));
    }
    return kj::Maybe(scriptTags.releaseAsArray());
  }
  return kj::none;
}

kj::Maybe<kj::String> getEntrypointFromReader(const rpc::Trace::Onset::Reader& reader) {
  if (reader.hasEntryPoint()) {
    return kj::str(reader.getEntryPoint());
  }
  return kj::none;
}
Onset::WorkerInfo getWorkerInfoFromReader(const rpc::Trace::Onset::Reader& reader) {
  return Onset::WorkerInfo{
    .executionModel = reader.getExecutionModel(),
    .scriptName = getScriptNameFromReader(reader),
    .scriptVersion = getScriptVersionFromReader(reader),
    .dispatchNamespace = getDispatchNamespaceFromReader(reader),
    .scriptId = getScriptIdFromReader(reader),
    .scriptTags = getScriptTagsFromReader(reader),
    .entrypoint = getEntrypointFromReader(reader),
  };
}
}  // namespace

Onset::Onset(
    SpanId spanId, Onset::Info&& info, Onset::WorkerInfo&& workerInfo, CustomInfo attributes)
    : spanId(spanId),
      info(kj::mv(info)),
      workerInfo(kj::mv(workerInfo)),
      attributes(kj::mv(attributes)) {}

Onset::Onset(rpc::Trace::Onset::Reader reader)
    : spanId(reader.getSpanId()),
      info(readOnsetInfo(reader.getInfo())),
      workerInfo(getWorkerInfoFromReader(reader)),
      attributes(KJ_MAP(attr, reader.getAttributes()) { return Attribute(attr); }) {}

void Onset::copyTo(rpc::Trace::Onset::Builder builder) const {
  builder.setExecutionModel(workerInfo.executionModel);
  builder.setSpanId(spanId);
  KJ_IF_SOME(name, workerInfo.scriptName) {
    builder.setScriptName(name);
  }
  KJ_IF_SOME(version, workerInfo.scriptVersion) {
    builder.setScriptVersion(*version);
  }
  KJ_IF_SOME(name, workerInfo.dispatchNamespace) {
    builder.setDispatchNamespace(name);
  }
  KJ_IF_SOME(scriptId, workerInfo.scriptId) {
    builder.setScriptId(scriptId);
  }
  KJ_IF_SOME(tags, workerInfo.scriptTags) {
    auto list = builder.initScriptTags(tags.size());
    for (size_t i = 0; i < tags.size(); i++) {
      list.set(i, tags[i]);
    }
  }
  KJ_IF_SOME(e, workerInfo.entrypoint) {
    builder.setEntryPoint(e);
  }
  auto infoBuilder = builder.initInfo();
  writeOnsetInfo(info, infoBuilder);

  auto attributeBuilder = builder.initAttributes(attributes.size());
  for (size_t n = 0; n < attributes.size(); n++) {
    attributes[n].copyTo(attributeBuilder[n]);
  }
}

Onset::WorkerInfo Onset::WorkerInfo::clone() const {
  return WorkerInfo{
    .executionModel = executionModel,
    .scriptName = mapCopyString(scriptName),
    .scriptVersion = scriptVersion.map([](auto& version) { return capnp::clone(*version); }),
    .dispatchNamespace = mapCopyString(dispatchNamespace),
    .scriptId = mapCopyString(scriptId),
    .scriptTags =
        scriptTags.map([](auto& tags) { return KJ_MAP(tag, tags) { return kj::str(tag); }; }),
    .entrypoint = mapCopyString(entrypoint),
  };
}

EventInfo cloneEventInfo(const EventInfo& info) {
  KJ_SWITCH_ONEOF(info) {
    KJ_CASE_ONEOF(fetch, FetchEventInfo) {
      return fetch.clone();
    }
    KJ_CASE_ONEOF(jsrpc, JsRpcEventInfo) {
      return jsrpc.clone();
    }
    KJ_CASE_ONEOF(scheduled, ScheduledEventInfo) {
      return scheduled.clone();
    }
    KJ_CASE_ONEOF(alarm, AlarmEventInfo) {
      return alarm.clone();
    }
    KJ_CASE_ONEOF(queue, QueueEventInfo) {
      return queue.clone();
    }
    KJ_CASE_ONEOF(email, EmailEventInfo) {
      return email.clone();
    }
    KJ_CASE_ONEOF(trace, TraceEventInfo) {
      return trace.clone();
    }
    KJ_CASE_ONEOF(hws, HibernatableWebSocketEventInfo) {
      return hws.clone();
    }
    KJ_CASE_ONEOF(custom, CustomEventInfo) {
      return CustomEventInfo();
    }
  }
  KJ_UNREACHABLE;
}

Onset Onset::clone() const {
  return Onset(spanId, cloneEventInfo(info), workerInfo.clone(),
      KJ_MAP(attr, attributes) { return attr.clone(); });
}

Outcome::Outcome(EventOutcome outcome, kj::Duration cpuTime, kj::Duration wallTime)
    : outcome(outcome),
      cpuTime(cpuTime),
      wallTime(wallTime) {}

Outcome::Outcome(rpc::Trace::Outcome::Reader reader)
    : outcome(reader.getOutcome()),
      cpuTime(reader.getCpuTime() * kj::MILLISECONDS),
      wallTime(reader.getWallTime() * kj::MILLISECONDS) {}

void Outcome::copyTo(rpc::Trace::Outcome::Builder builder) const {
  builder.setOutcome(outcome);
  builder.setCpuTime(cpuTime / kj::MILLISECONDS);
  builder.setWallTime(wallTime / kj::MILLISECONDS);
}

Outcome Outcome::clone() const {
  return Outcome(outcome, cpuTime, wallTime);
}

TailEvent::TailEvent(
    SpanContext context, TraceId invocationId, kj::Date timestamp, kj::uint sequence, Event&& event)
    : spanContext(kj::mv(context)),
      invocationId(invocationId),
      timestamp(timestamp),
      sequence(sequence),
      event(kj::mv(event)) {}

TailEvent::TailEvent(TraceId traceId,
    TraceId invocationId,
    kj::Maybe<SpanId> spanId,
    kj::Date timestamp,
    kj::uint sequence,
    Event&& event)
    : spanContext(kj::mv(traceId), kj::mv(spanId)),
      invocationId(kj::mv(invocationId)),
      timestamp(timestamp),
      sequence(sequence),
      event(kj::mv(event)) {}

namespace {
TailEvent::Event readEventFromTailEvent(const rpc::Trace::TailEvent::Reader& reader) {
  const auto event = reader.getEvent();
  switch (event.which()) {
    case rpc::Trace::TailEvent::Event::ONSET: {
      return Onset(event.getOnset());
    }
    case rpc::Trace::TailEvent::Event::OUTCOME: {
      return Outcome(event.getOutcome());
    }
    case rpc::Trace::TailEvent::Event::SPAN_OPEN: {
      return SpanOpen(event.getSpanOpen());
    }
    case rpc::Trace::TailEvent::Event::SPAN_CLOSE: {
      return SpanClose(event.getSpanClose());
    }
    case rpc::Trace::TailEvent::Event::ATTRIBUTE: {
      auto listReader = event.getAttribute();
      kj::Vector<Attribute> attrs(listReader.size());
      for (size_t n = 0; n < listReader.size(); n++) {
        attrs.add(Attribute(listReader[n]));
      }
      return attrs.releaseAsArray();
    }
    case rpc::Trace::TailEvent::Event::RETURN: {
      return Return(event.getReturn());
    }
    case rpc::Trace::TailEvent::Event::DIAGNOSTIC_CHANNEL_EVENT: {
      return DiagnosticChannelEvent(event.getDiagnosticChannelEvent());
    }
    case rpc::Trace::TailEvent::Event::EXCEPTION: {
      return Exception(event.getException());
    }
    case rpc::Trace::TailEvent::Event::LOG: {
      return Log(event.getLog());
    }
  }
  KJ_UNREACHABLE;
}
}  // namespace

TailEvent::TailEvent(rpc::Trace::TailEvent::Reader reader)
    : spanContext(SpanContext::fromCapnp(reader.getSpanContext())),
      invocationId(TraceId::fromCapnp(reader.getInvocationId())),
      timestamp(kj::UNIX_EPOCH + reader.getTimestampNs() * kj::NANOSECONDS),
      sequence(reader.getSequence()),
      event(readEventFromTailEvent(reader)) {}

void TailEvent::copyTo(rpc::Trace::TailEvent::Builder builder) const {
  spanContext.toCapnp(builder.initSpanContext());
  invocationId.toCapnp(builder.initInvocationId());
  builder.setTimestampNs((timestamp - kj::UNIX_EPOCH) / kj::NANOSECONDS);
  builder.setSequence(sequence);
  auto eventBuilder = builder.initEvent();
  KJ_SWITCH_ONEOF(event) {
    KJ_CASE_ONEOF(onset, Onset) {
      onset.copyTo(eventBuilder.initOnset());
    }
    KJ_CASE_ONEOF(outcome, Outcome) {
      outcome.copyTo(eventBuilder.initOutcome());
    }
    KJ_CASE_ONEOF(open, SpanOpen) {
      open.copyTo(eventBuilder.initSpanOpen());
    }
    KJ_CASE_ONEOF(close, SpanClose) {
      close.copyTo(eventBuilder.initSpanClose());
    }
    KJ_CASE_ONEOF(diag, DiagnosticChannelEvent) {
      diag.copyTo(eventBuilder.initDiagnosticChannelEvent());
    }
    KJ_CASE_ONEOF(ex, Exception) {
      ex.copyTo(eventBuilder.initException());
    }
    KJ_CASE_ONEOF(log, Log) {
      log.copyTo(eventBuilder.initLog());
    }
    KJ_CASE_ONEOF(ret, Return) {
      ret.copyTo(eventBuilder.initReturn());
    }
    KJ_CASE_ONEOF(attrs, CustomInfo) {
      // Mark is a collection of attributes.
      auto attrBuilder = eventBuilder.initAttribute(attrs.size());
      for (size_t n = 0; n < attrs.size(); n++) {
        attrs[n].copyTo(attrBuilder[n]);
      }
    }
  }
}

TailEvent TailEvent::clone() const {
  constexpr auto cloneEvent = [](const Event& event) -> Event {
    KJ_SWITCH_ONEOF(event) {
      KJ_CASE_ONEOF(onset, Onset) {
        return onset.clone();
      }
      KJ_CASE_ONEOF(outcome, Outcome) {
        return outcome.clone();
      }
      KJ_CASE_ONEOF(open, SpanOpen) {
        return open.clone();
      }
      KJ_CASE_ONEOF(close, SpanClose) {
        return close.clone();
      }
      KJ_CASE_ONEOF(diag, DiagnosticChannelEvent) {
        return diag.clone();
      }
      KJ_CASE_ONEOF(ex, Exception) {
        return ex.clone();
      }
      KJ_CASE_ONEOF(log, Log) {
        return log.clone();
      }
      KJ_CASE_ONEOF(ret, Return) {
        return ret.clone();
      }
      KJ_CASE_ONEOF(attrs, CustomInfo) {
        return KJ_MAP(attr, attrs) { return attr.clone(); };
      }
    }
    KJ_UNREACHABLE;
  };
  return TailEvent(spanContext.getTraceId(), invocationId, spanContext.getSpanId(), timestamp,
      sequence, cloneEvent(event));
}

void CompleteSpan::copyTo(rpc::UserSpanData::Builder builder) const {
  builder.setOperationName(operationName.asPtr());
  builder.setStartTimeNs((startTime - kj::UNIX_EPOCH) / kj::NANOSECONDS);
  builder.setEndTimeNs((endTime - kj::UNIX_EPOCH) / kj::NANOSECONDS);
  builder.setSpanId(spanId);
  builder.setParentSpanId(parentSpanId);

  auto tagsParam = builder.initTags(tags.size());
  auto i = 0;
  for (auto& tag: tags) {
    auto tagParam = tagsParam[i++];
    tagParam.setKey(tag.key.asPtr());
    serializeTagValue(tagParam.initValue(), tag.value);
  }
}

CompleteSpan::CompleteSpan(rpc::UserSpanData::Reader reader)
    : spanId(reader.getSpanId()),
      parentSpanId(reader.getParentSpanId()),
      operationName(kj::str(reader.getOperationName())),
      startTime(kj::UNIX_EPOCH + reader.getStartTimeNs() * kj::NANOSECONDS),
      endTime(kj::UNIX_EPOCH + reader.getEndTimeNs() * kj::NANOSECONDS) {
  auto tagsParam = reader.getTags();
  tags.reserve(tagsParam.size());
  for (auto tagParam: tagsParam) {
    tags.insert(kj::ConstString(kj::heapString(tagParam.getKey())),
        deserializeTagValue(tagParam.getValue()));
  }
}

CompleteSpan CompleteSpan::clone() const {
  CompleteSpan copy(spanId, parentSpanId, operationName.clone(), startTime, endTime);
  copy.tags.reserve(tags.size());
  for (auto& tag: tags) {
    copy.tags.insert(tag.key.clone(), spanTagClone(tag.value));
  }
  return copy;
}
}  // namespace tracing

// ======================================================================================

SpanBuilder::SpanBuilder(kj::Maybe<kj::Own<SpanObserver>> observer,
    kj::ConstString operationName,
    kj::Maybe<kj::Date> startTime) {
  KJ_IF_SOME(obs, observer) {
    // TODO(o11y): Once we report the user tracing spanOpen event as soon as a span is created, we
    // should be able to fold this virtual call and just get the timestamp directly.
    span.emplace(kj::mv(operationName), startTime.orDefault(obs->getTime()));
    this->observer = kj::mv(obs);
  }
}

SpanBuilder& SpanBuilder::operator=(SpanBuilder&& other) {
  end();
  observer = kj::mv(other.observer);
  span = kj::mv(other.span);
  return *this;
}

SpanBuilder::~SpanBuilder() noexcept(false) {
  end();
}

void SpanBuilder::end() {
  KJ_IF_SOME(o, observer) {
    KJ_IF_SOME(s, span) {
      // TODO(performance): Fold this timer call if we are using I/O time, where we will look up
      // I/O time later.
      s.endTime = kj::systemPreciseCalendarClock().now();
      o->report(s);
      span = kj::none;
    }
  }
}

void SpanBuilder::setOperationName(kj::ConstString operationName) {
  KJ_IF_SOME(s, span) {
    s.operationName = kj::mv(operationName);
  }
}

void SpanBuilder::setTag(kj::ConstString key, TagInitValue value) {
  KJ_IF_SOME(s, span) {
    // We allow passing a LiteralStringConst or StringPtr so that we don't have to allocate memory
    // if we're not being observed.
    TagValue v = [](TagInitValue value) -> Span::TagValue {
      KJ_SWITCH_ONEOF(value) {
        KJ_CASE_ONEOF(str, kj::StringPtr) {
          return kj::ConstString(kj::str(str));
        }
        KJ_CASE_ONEOF(str, kj::LiteralStringConst) {
          return kj::ConstString(str);
        }
        KJ_CASE_ONEOF(str, kj::ConstString) {
          return kj::mv(str);
        }
        KJ_CASE_ONEOF(str, kj::String) {
          return kj::ConstString(kj::mv(str));
        }
        KJ_CASE_ONEOF(val, int64_t) {
          return val;
        }
        KJ_CASE_ONEOF(val, double) {
          return val;
        }
        KJ_CASE_ONEOF(val, bool) {
          return val;
        }
      }
      KJ_UNREACHABLE;
    }(kj::mv(value));

    auto keyPtr = key.asPtr();
    s.tags.upsert(kj::mv(key), kj::mv(v), [keyPtr](TagValue& existingValue, TagValue&& newValue) {
      // This is a programming error, but not a serious one. We could alternatively just emit
      // duplicate tags and leave the Jaeger UI in charge of warning about them.
      [[maybe_unused]] static auto logged = [keyPtr]() {
        if (isPredictableModeForTest()) {
          // Logging in ERROR level to have this fail loudly during testing.
          KJ_LOG(ERROR, "overwriting previous tag", keyPtr);
        } else {
          KJ_LOG(WARNING, "overwriting previous tag", keyPtr);
        }
        return true;
      }();
      existingValue = kj::mv(newValue);
    });
  }
}

void SpanBuilder::addLog(kj::Date timestamp, kj::ConstString key, TagValue value) {
  KJ_IF_SOME(s, span) {
    if (s.logs.size() >= Span::MAX_LOGS) {
      ++s.droppedLogs;
    } else {
      s.logs.add(Span::Log{.timestamp = timestamp,
        .tag = {
          .key = kj::mv(key),
          .value = kj::mv(value),
        }});
    }
  }
}

void TraceContext::setTag(kj::ConstString key, SpanBuilder::TagInitValue value) {
  // Fast path (without string allocations) if only some spans are observed.
  if (!span.isObserved()) {
    userSpan.setTag(kj::mv(key), kj::mv(value));
    return;
  }
  if (!userSpan.isObserved()) {
    span.setTag(kj::mv(key), kj::mv(value));
    return;
  }

  // We need to duplicate the key and value since both are move-only types.
  // Clone the value based on its type.
  KJ_SWITCH_ONEOF(value) {
    KJ_CASE_ONEOF(s, kj::StringPtr) {
      span.setTag(key.clone(), s);
      userSpan.setTag(kj::mv(key), s);
    }
    KJ_CASE_ONEOF(s, kj::String) {
      span.setTag(key.clone(), kj::str(s));
      userSpan.setTag(kj::mv(key), kj::mv(s));
    }
    KJ_CASE_ONEOF(s, kj::LiteralStringConst) {
      span.setTag(key.clone(), s);
      userSpan.setTag(kj::mv(key), s);
    }
    KJ_CASE_ONEOF(s, kj::ConstString) {
      span.setTag(key.clone(), s.clone());
      userSpan.setTag(kj::mv(key), kj::mv(s));
    }
    KJ_CASE_ONEOF(b, bool) {
      span.setTag(key.clone(), b);
      userSpan.setTag(kj::mv(key), b);
    }
    KJ_CASE_ONEOF(d, double) {
      span.setTag(key.clone(), d);
      userSpan.setTag(kj::mv(key), d);
    }
    KJ_CASE_ONEOF(i, int64_t) {
      span.setTag(key.clone(), i);
      userSpan.setTag(kj::mv(key), i);
    }
  }
}

Span::TagValue spanTagClone(const Span::TagValue& tag) {
  KJ_SWITCH_ONEOF(tag) {
    KJ_CASE_ONEOF(str, kj::ConstString) {
      return str.clone();
    }
    KJ_CASE_ONEOF(val, int64_t) {
      return val;
    }
    KJ_CASE_ONEOF(val, double) {
      return val;
    }
    KJ_CASE_ONEOF(val, bool) {
      return val;
    }
  }
  KJ_UNREACHABLE;
}

using RpcValue = rpc::TagValue;
void serializeTagValue(RpcValue::Builder builder, const Span::TagValue& value) {
  KJ_SWITCH_ONEOF(value) {
    KJ_CASE_ONEOF(b, bool) {
      builder.setBool(b);
    }
    KJ_CASE_ONEOF(i, int64_t) {
      builder.setInt64(i);
    }
    KJ_CASE_ONEOF(d, double) {
      builder.setFloat64(d);
    }
    KJ_CASE_ONEOF(s, kj::ConstString) {
      builder.setString(s.asPtr());
    }
  }
}

Span::TagValue deserializeTagValue(RpcValue::Reader value) {
  switch (value.which()) {
    case RpcValue::BOOL:
      return value.getBool();
    case RpcValue::FLOAT64:
      return value.getFloat64();
    case RpcValue::INT64:
      return value.getInt64();
    case RpcValue::STRING:
      return kj::ConstString(kj::heapString(value.getString()));
    default:
      KJ_UNREACHABLE;
  }
}

ScopedDurationTagger::ScopedDurationTagger(
    SpanBuilder& span, kj::ConstString key, const kj::MonotonicClock& timer)
    : span(span),
      key(kj::mv(key)),
      timer(timer),
      startTime(timer.now()) {}

ScopedDurationTagger::~ScopedDurationTagger() noexcept(false) {
  auto duration = timer.now() - startTime;
  if (isPredictableModeForTest()) {
    duration = 0 * kj::NANOSECONDS;
  }
  span.setTag(kj::mv(key), duration / kj::NANOSECONDS);
}

}  // namespace workerd
