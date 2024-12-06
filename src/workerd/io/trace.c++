// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/io/trace.h>
#include <workerd/util/thread-scopes.h>

#include <openssl/rand.h>

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
      entropy.generate(kj::arrayPtr(&ret, 1).asBytes());
    } else {
      KJ_ASSERT(RAND_bytes(reinterpret_cast<uint8_t*>(&ret), sizeof(ret)) == 1);
    }
    // On the extreme off chance that we ended with with zeroes
    // let's try again, but only up to three times.
  } while (ret == 0 && tries < 3);

  return ret;
}
}  // namespace

TraceId TraceId::fromEntropy(kj::Maybe<kj::EntropySource&> entropySource) {
  if (isPredictableModeForTest()) {
    return TraceId(0x2a2a2a2a2a2a2a2a, 0x2a2a2a2a2a2a2a2a);
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
                     .map([&](auto& ctx) mutable -> TraceId {
    parent = ctx;
    return ctx.traceId;
  }).orDefault([&] { return TraceId::fromEntropy(entropySource); });
  return InvocationSpanContext(kj::Badge<InvocationSpanContext>(), entropySource, kj::mv(traceId),
      TraceId::fromEntropy(entropySource), SpanId::fromEntropy(entropySource), kj::mv(parent));
}

TraceId TraceId::fromCapnp(rpc::InvocationSpanContext::TraceId::Reader reader) {
  return TraceId(reader.getLow(), reader.getHigh());
}

void TraceId::toCapnp(rpc::InvocationSpanContext::TraceId::Builder writer) const {
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

}  // namespace tracing

// Approximately how much external data we allow in a trace before we start ignoring requests.  We
// want this number to be big enough to be useful for tracing, but small enough to make it hard to
// DoS the C++ heap -- keeping in mind we can record a trace per handler run during a request.
static constexpr size_t MAX_TRACE_BYTES = 128 * 1024;
// Limit spans to at most 512, it could be difficult to fit e.g. 1024 spans within MAX_TRACE_BYTES
// unless most of the included spans do not include tags. If use cases arise where this amount is
// insufficient, merge smaller spans together or drop smaller spans.
static constexpr size_t MAX_USER_SPANS = 512;

namespace {

static kj::HttpMethod validateMethod(capnp::HttpMethod method) {
  KJ_REQUIRE(method <= capnp::HttpMethod::UNSUBSCRIBE, "unknown method", method);
  return static_cast<kj::HttpMethod>(method);
}

}  // namespace

tracing::FetchEventInfo::FetchEventInfo(
    kj::HttpMethod method, kj::String url, kj::String cfJson, kj::Array<Header> headers)
    : method(method),
      url(kj::mv(url)),
      cfJson(kj::mv(cfJson)),
      headers(kj::mv(headers)) {}

tracing::FetchEventInfo::FetchEventInfo(rpc::Trace::FetchEventInfo::Reader reader)
    : method(validateMethod(reader.getMethod())),
      url(kj::str(reader.getUrl())),
      cfJson(kj::str(reader.getCfJson())) {
  kj::Vector<Header> v;
  v.addAll(reader.getHeaders());
  headers = v.releaseAsArray();
}

void tracing::FetchEventInfo::copyTo(rpc::Trace::FetchEventInfo::Builder builder) {
  builder.setMethod(static_cast<capnp::HttpMethod>(method));
  builder.setUrl(url);
  builder.setCfJson(cfJson);

  auto list = builder.initHeaders(headers.size());
  for (auto i: kj::indices(headers)) {
    headers[i].copyTo(list[i]);
  }
}

tracing::FetchEventInfo tracing::FetchEventInfo::clone() {
  return FetchEventInfo(
      method, kj::str(url), kj::str(cfJson), KJ_MAP(h, headers) { return h.clone(); });
}

tracing::FetchEventInfo::Header::Header(kj::String name, kj::String value)
    : name(kj::mv(name)),
      value(kj::mv(value)) {}

tracing::FetchEventInfo::Header::Header(rpc::Trace::FetchEventInfo::Header::Reader reader)
    : name(kj::str(reader.getName())),
      value(kj::str(reader.getValue())) {}

void tracing::FetchEventInfo::Header::copyTo(rpc::Trace::FetchEventInfo::Header::Builder builder) {
  builder.setName(name);
  builder.setValue(value);
}

tracing::FetchEventInfo::Header tracing::FetchEventInfo::Header::clone() {
  return Header(kj::str(name), kj::str(value));
}

tracing::JsRpcEventInfo::JsRpcEventInfo(kj::String methodName): methodName(kj::mv(methodName)) {}

tracing::JsRpcEventInfo::JsRpcEventInfo(rpc::Trace::JsRpcEventInfo::Reader reader)
    : methodName(kj::str(reader.getMethodName())) {}

void tracing::JsRpcEventInfo::copyTo(rpc::Trace::JsRpcEventInfo::Builder builder) {
  builder.setMethodName(methodName);
}

tracing::JsRpcEventInfo tracing::JsRpcEventInfo::clone() {
  return JsRpcEventInfo(kj::str(methodName));
}

tracing::ScheduledEventInfo::ScheduledEventInfo(double scheduledTime, kj::String cron)
    : scheduledTime(scheduledTime),
      cron(kj::mv(cron)) {}

tracing::ScheduledEventInfo::ScheduledEventInfo(rpc::Trace::ScheduledEventInfo::Reader reader)
    : scheduledTime(reader.getScheduledTime()),
      cron(kj::str(reader.getCron())) {}

void tracing::ScheduledEventInfo::copyTo(rpc::Trace::ScheduledEventInfo::Builder builder) {
  builder.setScheduledTime(scheduledTime);
  builder.setCron(cron);
}

tracing::ScheduledEventInfo tracing::ScheduledEventInfo::clone() {
  return ScheduledEventInfo(scheduledTime, kj::str(cron));
}

tracing::AlarmEventInfo::AlarmEventInfo(kj::Date scheduledTime): scheduledTime(scheduledTime) {}

tracing::AlarmEventInfo::AlarmEventInfo(rpc::Trace::AlarmEventInfo::Reader reader)
    : scheduledTime(reader.getScheduledTimeMs() * kj::MILLISECONDS + kj::UNIX_EPOCH) {}

void tracing::AlarmEventInfo::copyTo(rpc::Trace::AlarmEventInfo::Builder builder) {
  builder.setScheduledTimeMs((scheduledTime - kj::UNIX_EPOCH) / kj::MILLISECONDS);
}

tracing::AlarmEventInfo tracing::AlarmEventInfo::clone() {
  return AlarmEventInfo(scheduledTime);
}

tracing::QueueEventInfo::QueueEventInfo(kj::String queueName, uint32_t batchSize)
    : queueName(kj::mv(queueName)),
      batchSize(batchSize) {}

tracing::QueueEventInfo::QueueEventInfo(rpc::Trace::QueueEventInfo::Reader reader)
    : queueName(kj::heapString(reader.getQueueName())),
      batchSize(reader.getBatchSize()) {}

void tracing::QueueEventInfo::copyTo(rpc::Trace::QueueEventInfo::Builder builder) {
  builder.setQueueName(queueName);
  builder.setBatchSize(batchSize);
}

tracing::QueueEventInfo tracing::QueueEventInfo::clone() {
  return QueueEventInfo(kj::str(queueName), batchSize);
}

tracing::EmailEventInfo::EmailEventInfo(kj::String mailFrom, kj::String rcptTo, uint32_t rawSize)
    : mailFrom(kj::mv(mailFrom)),
      rcptTo(kj::mv(rcptTo)),
      rawSize(rawSize) {}

tracing::EmailEventInfo::EmailEventInfo(rpc::Trace::EmailEventInfo::Reader reader)
    : mailFrom(kj::heapString(reader.getMailFrom())),
      rcptTo(kj::heapString(reader.getRcptTo())),
      rawSize(reader.getRawSize()) {}

void tracing::EmailEventInfo::copyTo(rpc::Trace::EmailEventInfo::Builder builder) {
  builder.setMailFrom(mailFrom);
  builder.setRcptTo(rcptTo);
  builder.setRawSize(rawSize);
}

tracing::EmailEventInfo tracing::EmailEventInfo::clone() {
  return EmailEventInfo(kj::str(mailFrom), kj::str(rcptTo), rawSize);
}

kj::Vector<tracing::TraceEventInfo::TraceItem> getTraceItemsFromTraces(
    kj::ArrayPtr<kj::Own<Trace>> traces) {
  return KJ_MAP(t, traces) -> tracing::TraceEventInfo::TraceItem {
    return tracing::TraceEventInfo::TraceItem(
        t->scriptName.map([](auto& scriptName) { return kj::str(scriptName); }));
  };
}

tracing::TraceEventInfo::TraceEventInfo(kj::ArrayPtr<kj::Own<Trace>> traces)
    : traces(getTraceItemsFromTraces(traces)) {}

kj::Vector<tracing::TraceEventInfo::TraceItem> getTraceItemsFromReader(
    rpc::Trace::TraceEventInfo::Reader reader) {
  return KJ_MAP(r, reader.getTraces()) -> tracing::TraceEventInfo::TraceItem {
    return tracing::TraceEventInfo::TraceItem(r);
  };
}

tracing::TraceEventInfo::TraceEventInfo(rpc::Trace::TraceEventInfo::Reader reader)
    : traces(getTraceItemsFromReader(reader)) {}

void tracing::TraceEventInfo::copyTo(rpc::Trace::TraceEventInfo::Builder builder) {
  auto list = builder.initTraces(traces.size());
  for (auto i: kj::indices(traces)) {
    traces[i].copyTo(list[i]);
  }
}

tracing::TraceEventInfo tracing::TraceEventInfo::clone() {
  return TraceEventInfo(KJ_MAP(item, traces) { return item.clone(); });
}

tracing::TraceEventInfo::TraceItem::TraceItem(kj::Maybe<kj::String> scriptName)
    : scriptName(kj::mv(scriptName)) {}

tracing::TraceEventInfo::TraceItem::TraceItem(rpc::Trace::TraceEventInfo::TraceItem::Reader reader)
    : scriptName(kj::str(reader.getScriptName())) {}

void tracing::TraceEventInfo::TraceItem::copyTo(
    rpc::Trace::TraceEventInfo::TraceItem::Builder builder) {
  KJ_IF_SOME(name, scriptName) {
    builder.setScriptName(name);
  }
}

tracing::TraceEventInfo::TraceItem tracing::TraceEventInfo::TraceItem::clone() {
  return TraceItem(scriptName.map([](auto& name) { return kj::str(name); }));
}

tracing::DiagnosticChannelEvent::DiagnosticChannelEvent(
    kj::Date timestamp, kj::String channel, kj::Array<kj::byte> message)
    : timestamp(timestamp),
      channel(kj::mv(channel)),
      message(kj::mv(message)) {}

tracing::DiagnosticChannelEvent::DiagnosticChannelEvent(
    rpc::Trace::DiagnosticChannelEvent::Reader reader)
    : timestamp(kj::UNIX_EPOCH + reader.getTimestampNs() * kj::NANOSECONDS),
      channel(kj::heapString(reader.getChannel())),
      message(kj::heapArray<kj::byte>(reader.getMessage())) {}

void tracing::DiagnosticChannelEvent::copyTo(rpc::Trace::DiagnosticChannelEvent::Builder builder) {
  builder.setTimestampNs((timestamp - kj::UNIX_EPOCH) / kj::NANOSECONDS);
  builder.setChannel(channel);
  builder.setMessage(message);
}

tracing::DiagnosticChannelEvent tracing::DiagnosticChannelEvent::clone() {
  return DiagnosticChannelEvent(timestamp, kj::str(channel), kj::heapArray<kj::byte>(message));
}

tracing::HibernatableWebSocketEventInfo::HibernatableWebSocketEventInfo(Type type): type(type) {}

tracing::HibernatableWebSocketEventInfo::HibernatableWebSocketEventInfo(
    rpc::Trace::HibernatableWebSocketEventInfo::Reader reader)
    : type(readFrom(reader)) {}

void tracing::HibernatableWebSocketEventInfo::copyTo(
    rpc::Trace::HibernatableWebSocketEventInfo::Builder builder) {
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

tracing::HibernatableWebSocketEventInfo tracing::HibernatableWebSocketEventInfo::clone() {
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

tracing::HibernatableWebSocketEventInfo::Type tracing::HibernatableWebSocketEventInfo::readFrom(
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

tracing::FetchResponseInfo::FetchResponseInfo(uint16_t statusCode): statusCode(statusCode) {}

tracing::FetchResponseInfo::FetchResponseInfo(rpc::Trace::FetchResponseInfo::Reader reader)
    : statusCode(reader.getStatusCode()) {}

void tracing::FetchResponseInfo::copyTo(rpc::Trace::FetchResponseInfo::Builder builder) {
  builder.setStatusCode(statusCode);
}

tracing::FetchResponseInfo tracing::FetchResponseInfo::clone() {
  return FetchResponseInfo(statusCode);
}

tracing::Log::Log(kj::Date timestamp, LogLevel logLevel, kj::String message)
    : timestamp(timestamp),
      logLevel(logLevel),
      message(kj::mv(message)) {}

tracing::Exception::Exception(
    kj::Date timestamp, kj::String name, kj::String message, kj::Maybe<kj::String> stack)
    : timestamp(timestamp),
      name(kj::mv(name)),
      message(kj::mv(message)),
      stack(kj::mv(stack)) {}

Trace::Trace(kj::Maybe<kj::String> stableId,
    kj::Maybe<kj::String> scriptName,
    kj::Maybe<kj::Own<ScriptVersion::Reader>> scriptVersion,
    kj::Maybe<kj::String> dispatchNamespace,
    kj::Maybe<kj::String> scriptId,
    kj::Array<kj::String> scriptTags,
    kj::Maybe<kj::String> entrypoint,
    ExecutionModel executionModel)
    : stableId(kj::mv(stableId)),
      scriptName(kj::mv(scriptName)),
      scriptVersion(kj::mv(scriptVersion)),
      dispatchNamespace(kj::mv(dispatchNamespace)),
      scriptId(kj::mv(scriptId)),
      scriptTags(kj::mv(scriptTags)),
      entrypoint(kj::mv(entrypoint)),
      executionModel(executionModel) {}
Trace::Trace(rpc::Trace::Reader reader) {
  mergeFrom(reader, PipelineLogLevel::FULL);
}

Trace::~Trace() noexcept(false) {}

void Trace::copyTo(rpc::Trace::Builder builder) {
  {
    auto list = builder.initLogs(logs.size() + spans.size());
    for (auto i: kj::indices(logs)) {
      logs[i].copyTo(list[i]);
    }
    // Add spans represented as logs to the logs object.
    for (auto i: kj::indices(spans)) {
      spans[i].copyTo(list[i + logs.size()]);
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
      KJ_CASE_ONEOF(resume, tracing::Resume) {
        // Resume is not used in legacy trace.
        KJ_UNREACHABLE;
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

void tracing::Log::copyTo(rpc::Trace::Log::Builder builder) {
  builder.setTimestampNs((timestamp - kj::UNIX_EPOCH) / kj::NANOSECONDS);
  builder.setLogLevel(logLevel);
  builder.setMessage(message);
}

tracing::Log tracing::Log::clone() {
  return Log(timestamp, logLevel, kj::str(message));
}

void tracing::Exception::copyTo(rpc::Trace::Exception::Builder builder) {
  builder.setTimestampNs((timestamp - kj::UNIX_EPOCH) / kj::NANOSECONDS);
  builder.setName(name);
  builder.setMessage(message);
  KJ_IF_SOME(s, stack) {
    builder.setStack(s);
  }
}

tracing::Exception tracing::Exception::clone() {
  return Exception(timestamp, kj::str(name), kj::str(message),
      stack.map([](auto& stack) { return kj::str(stack); }));
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

tracing::Log::Log(rpc::Trace::Log::Reader reader)
    : timestamp(kj::UNIX_EPOCH + reader.getTimestampNs() * kj::NANOSECONDS),
      logLevel(reader.getLogLevel()),
      message(kj::str(reader.getMessage())) {}
tracing::Exception::Exception(rpc::Trace::Exception::Reader reader)
    : timestamp(kj::UNIX_EPOCH + reader.getTimestampNs() * kj::NANOSECONDS),
      name(kj::str(reader.getName())),
      message(kj::str(reader.getMessage())) {
  if (reader.hasStack()) {
    stack = kj::str(reader.getStack());
  }
}

namespace {
kj::Maybe<kj::Array<kj::byte>> readResumeAttachment(const auto& reader) {
  if (reader.hasAttachment()) {
    return kj::heapArray<kj::byte>(reader.getAttachment());
  }
  return kj::none;
}
}  // namespace

tracing::Resume::Resume(kj::Maybe<kj::Array<kj::byte>> attachment)
    : attachment(kj::mv(attachment)) {}

tracing::Resume::Resume(rpc::Trace::Resume::Reader reader)
    : attachment(readResumeAttachment(reader)) {}

void tracing::Resume::copyTo(rpc::Trace::Resume::Builder builder) {
  KJ_IF_SOME(attach, attachment) {
    builder.setAttachment(attach);
  }
}

tracing::Resume tracing::Resume::clone() {
  return Resume(attachment.map([](auto& attach) { return kj::heapArray<kj::byte>(attach); }));
}

tracing::Hibernate::Hibernate() {}

tracing::Hibernate::Hibernate(rpc::Trace::Hibernate::Reader reader) {}

void tracing::Hibernate::copyTo(rpc::Trace::Hibernate::Builder builder) {}

tracing::Hibernate tracing::Hibernate::clone() {
  return Hibernate();
}

tracing::Attribute::Attribute(kj::String name, Value&& value)
    : name(kj::mv(name)),
      value(kj::mv(value)) {}

tracing::Attribute::Attribute(kj::String name, kj::Array<Value>&& value)
    : name(kj::mv(name)),
      value(kj::mv(value)) {}

namespace {
kj::OneOf<tracing::Attribute::Value, kj::Array<tracing::Attribute::Value>> readValues(
    const rpc::Trace::Attribute::Reader& reader) {
  static auto readValue =
      [](rpc::Trace::Attribute::Value::Reader reader) -> tracing::Attribute::Value {
    auto inner = reader.getInner();
    switch (inner.which()) {
      case rpc::Trace::Attribute::Value::Inner::TEXT: {
        return kj::str(inner.getText());
      }
      case rpc::Trace::Attribute::Value::Inner::BOOL: {
        return inner.getBool();
      }
      case rpc::Trace::Attribute::Value::Inner::FLOAT: {
        return inner.getFloat();
      }
      case rpc::Trace::Attribute::Value::Inner::INT: {
        return static_cast<kj::uint>(inner.getInt());
      }
    }
    KJ_UNREACHABLE;
  };

  // There should always be a value and it always have at least one entry in the list.
  KJ_ASSERT(reader.hasValue());
  auto value = reader.getValue();
  KJ_ASSERT(value.size() >= 1);
  if (value.size() == 1) {
    return readValue(value[0]);
  }
  kj::Vector<tracing::Attribute::Value> values(value.size());
  for (auto v: value) {
    values.add(readValue(v));
  }
  return values.releaseAsArray();
}
}  // namespace

tracing::Attribute::Attribute(rpc::Trace::Attribute::Reader reader)
    : name(kj::str(reader.getName())),
      value(readValues(reader)) {}

void tracing::Attribute::copyTo(rpc::Trace::Attribute::Builder builder) {
  static auto writeValue = [](auto builder, const auto& value) mutable {
    KJ_SWITCH_ONEOF(value) {
      KJ_CASE_ONEOF(str, kj::String) {
        builder.initInner().setText(str.asPtr());
      }
      KJ_CASE_ONEOF(b, bool) {
        builder.initInner().setBool(b);
      }
      KJ_CASE_ONEOF(f, double) {
        builder.initInner().setFloat(f);
      }
      KJ_CASE_ONEOF(i, uint32_t) {
        builder.initInner().setInt(i);
      }
    }
  };
  builder.setName(name.asPtr());
  KJ_SWITCH_ONEOF(value) {
    KJ_CASE_ONEOF(value, Value) {
      writeValue(builder.initValue(1)[0], value);
    }
    KJ_CASE_ONEOF(values, kj::Array<Value>) {
      auto vec = builder.initValue(values.size());
      for (size_t n = 0; n < values.size(); n++) {
        writeValue(vec[n], values[n]);
      }
    }
  }
}

tracing::Attribute tracing::Attribute::clone() {
  constexpr auto cloneValue = [](const Value& value) -> Value {
    KJ_SWITCH_ONEOF(value) {
      KJ_CASE_ONEOF(str, kj::String) {
        return kj::str(str);
      }
      KJ_CASE_ONEOF(b, bool) {
        return b;
      }
      KJ_CASE_ONEOF(f, double) {
        return f;
      }
      KJ_CASE_ONEOF(i, uint32_t) {
        return i;
      }
    }
    KJ_UNREACHABLE;
  };

  KJ_SWITCH_ONEOF(value) {
    KJ_CASE_ONEOF(value, Value) {
      return Attribute(kj::str(name), cloneValue(value));
    }
    KJ_CASE_ONEOF(values, kj::Array<Value>) {
      return Attribute(kj::str(name), KJ_MAP(value, values) { return cloneValue(value); });
    }
  }
  KJ_UNREACHABLE;
}

tracing::Return::Return(kj::Maybe<tracing::Return::Info> info): info(kj::mv(info)) {}

namespace {
kj::Maybe<tracing::Return::Info> readReturnInfo(const rpc::Trace::Return::Reader& reader) {
  auto info = reader.getInfo();
  switch (info.which()) {
    case rpc::Trace::Return::Info::EMPTY:
      return kj::none;
    case rpc::Trace::Return::Info::CUSTOM: {
      auto list = info.getCustom();
      kj::Vector<tracing::Attribute> attrs(list.size());
      for (size_t n = 0; n < list.size(); n++) {
        attrs.add(tracing::Attribute(list[n]));
      }
      return kj::Maybe(attrs.releaseAsArray());
    }
    case rpc::Trace::Return::Info::FETCH: {
      return kj::Maybe(tracing::FetchResponseInfo(info.getFetch()));
    }
  }
  KJ_UNREACHABLE;
}
}  // namespace

tracing::Return::Return(rpc::Trace::Return::Reader reader): info(readReturnInfo(reader)) {}

void tracing::Return::copyTo(rpc::Trace::Return::Builder builder) {
  KJ_IF_SOME(i, info) {
    auto infoBuilder = builder.initInfo();
    KJ_SWITCH_ONEOF(i) {
      KJ_CASE_ONEOF(fetch, tracing::FetchResponseInfo) {
        fetch.copyTo(infoBuilder.initFetch());
      }
      KJ_CASE_ONEOF(custom, tracing::CustomInfo) {
        auto attributes = infoBuilder.initCustom(custom.size());
        for (size_t n = 0; n < custom.size(); n++) {
          custom[n].copyTo(attributes[n]);
        }
      }
    }
  }
}

tracing::Return tracing::Return::clone() {
  KJ_IF_SOME(i, info) {
    KJ_SWITCH_ONEOF(i) {
      KJ_CASE_ONEOF(fetch, tracing::FetchResponseInfo) {
        return Return(kj::Maybe(fetch.clone()));
      }
      KJ_CASE_ONEOF(custom, tracing::CustomInfo) {
        return Return(kj::Maybe(KJ_MAP(i, custom) { return i.clone(); }));
      }
    }
    KJ_UNREACHABLE;
  }
  return Return();
}

tracing::SpanOpen::SpanOpen(kj::Maybe<kj::String> operationName, kj::Maybe<Info> info)
    : operationName(kj::mv(operationName)),
      info(kj::mv(info)) {}

namespace {
kj::Maybe<kj::String> readSpanOpenOperationName(const rpc::Trace::SpanOpen::Reader& reader) {
  if (!reader.hasOperationName()) return kj::none;
  return kj::str(reader.getOperationName());
}

kj::Maybe<tracing::SpanOpen::Info> readSpanOpenInfo(rpc::Trace::SpanOpen::Reader& reader) {
  auto info = reader.getInfo();
  switch (info.which()) {
    case rpc::Trace::SpanOpen::Info::EMPTY:
      return kj::none;
    case rpc::Trace::SpanOpen::Info::FETCH: {
      return kj::Maybe(tracing::FetchEventInfo(info.getFetch()));
    }
    case rpc::Trace::SpanOpen::Info::JSRPC: {
      return kj::Maybe(tracing::JsRpcEventInfo(info.getJsrpc()));
    }
    case rpc::Trace::SpanOpen::Info::CUSTOM: {
      auto custom = info.getCustom();
      kj::Vector<tracing::Attribute> attrs(custom.size());
      for (size_t n = 0; n < custom.size(); n++) {
        attrs.add(tracing::Attribute(custom[n]));
      }
      return kj::Maybe(attrs.releaseAsArray());
    }
  }
  KJ_UNREACHABLE;
}
}  // namespace

tracing::SpanOpen::SpanOpen(rpc::Trace::SpanOpen::Reader reader)
    : operationName(readSpanOpenOperationName(reader)),
      info(readSpanOpenInfo(reader)) {}

void tracing::SpanOpen::copyTo(rpc::Trace::SpanOpen::Builder builder) {
  KJ_IF_SOME(name, operationName) {
    builder.setOperationName(name.asPtr());
  }
  KJ_IF_SOME(i, info) {
    auto infoBuilder = builder.initInfo();
    KJ_SWITCH_ONEOF(i) {
      KJ_CASE_ONEOF(fetch, tracing::FetchEventInfo) {
        fetch.copyTo(infoBuilder.initFetch());
      }
      KJ_CASE_ONEOF(jsrpc, tracing::JsRpcEventInfo) {
        jsrpc.copyTo(infoBuilder.initJsrpc());
      }
      KJ_CASE_ONEOF(custom, tracing::CustomInfo) {
        auto customBuilder = infoBuilder.initCustom(custom.size());
        for (size_t n = 0; n < custom.size(); n++) {
          custom[n].copyTo(customBuilder[n]);
        }
      }
    }
  }
}

tracing::SpanOpen tracing::SpanOpen::clone() {
  constexpr auto cloneInfo = [](kj::Maybe<Info>& info) -> kj::Maybe<tracing::SpanOpen::Info> {
    return info.map([](Info& info) -> tracing::SpanOpen::Info {
      KJ_SWITCH_ONEOF(info) {
        KJ_CASE_ONEOF(fetch, tracing::FetchEventInfo) {
          return fetch.clone();
        }
        KJ_CASE_ONEOF(jsrpc, tracing::JsRpcEventInfo) {
          return jsrpc.clone();
        }
        KJ_CASE_ONEOF(custom, tracing::CustomInfo) {
          kj::Vector<tracing::Attribute> attrs(custom.size());
          for (size_t n = 0; n < custom.size(); n++) {
            attrs.add(custom[n].clone());
          }
          return attrs.releaseAsArray();
        }
      }
      KJ_UNREACHABLE;
    });
  };
  return SpanOpen(operationName.map([](auto& str) { return kj::str(str); }), cloneInfo(info));
}

tracing::SpanClose::SpanClose(EventOutcome outcome): outcome(outcome) {}

tracing::SpanClose::SpanClose(rpc::Trace::SpanClose::Reader reader): outcome(reader.getOutcome()) {}

void tracing::SpanClose::copyTo(rpc::Trace::SpanClose::Builder builder) {
  builder.setOutcome(outcome);
}

tracing::SpanClose tracing::SpanClose::clone() {
  return SpanClose(outcome);
}

namespace {
tracing::Onset::Info getInfoFromReader(const rpc::Trace::Onset::Reader& reader) {
  auto info = reader.getInfo();
  switch (info.which()) {
    case rpc::Trace::Onset::Info::FETCH: {
      return tracing::FetchEventInfo(info.getFetch());
    }
    case rpc::Trace::Onset::Info::JSRPC: {
      return tracing::JsRpcEventInfo(info.getJsrpc());
    }
    case rpc::Trace::Onset::Info::SCHEDULED: {
      return tracing::ScheduledEventInfo(info.getScheduled());
    }
    case rpc::Trace::Onset::Info::ALARM: {
      return tracing::AlarmEventInfo(info.getAlarm());
    }
    case rpc::Trace::Onset::Info::QUEUE: {
      return tracing::QueueEventInfo(info.getQueue());
    }
    case rpc::Trace::Onset::Info::EMAIL: {
      return tracing::EmailEventInfo(info.getEmail());
    }
    case rpc::Trace::Onset::Info::TRACE: {
      return tracing::TraceEventInfo(info.getTrace());
    }
    case rpc::Trace::Onset::Info::HIBERNATABLE_WEB_SOCKET: {
      return tracing::HibernatableWebSocketEventInfo(info.getHibernatableWebSocket());
    }
    case rpc::Trace::Onset::Info::RESUME: {
      return tracing::Resume(info.getResume());
    }
    case rpc::Trace::Onset::Info::CUSTOM: {
      return tracing::CustomEventInfo();
    }
  }
  KJ_UNREACHABLE;
}

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
}  // namespace

tracing::Onset::Onset(tracing::Onset::Info&& info,
    ExecutionModel executionModel,
    kj::Maybe<kj::String> scriptName,
    kj::Maybe<kj::Own<ScriptVersion::Reader>> scriptVersion,
    kj::Maybe<kj::String> dispatchNamespace,
    kj::Maybe<kj::Array<kj::String>> scriptTags,
    kj::Maybe<kj::String> entrypoint)
    : info(kj::mv(info)),
      executionModel(executionModel),
      scriptName(kj::mv(scriptName)),
      scriptVersion(kj::mv(scriptVersion)),
      dispatchNamespace(kj::mv(dispatchNamespace)),
      scriptTags(kj::mv(scriptTags)),
      entrypoint(kj::mv(entrypoint)) {}

tracing::Onset::Onset(rpc::Trace::Onset::Reader reader)
    : info(getInfoFromReader(reader)),
      executionModel(reader.getExecutionModel()),
      scriptName(getScriptNameFromReader(reader)),
      scriptVersion(getScriptVersionFromReader(reader)),
      dispatchNamespace(getDispatchNamespaceFromReader(reader)),
      scriptTags(getScriptTagsFromReader(reader)),
      entrypoint(getEntrypointFromReader(reader)) {}

void tracing::Onset::copyTo(rpc::Trace::Onset::Builder builder) {
  builder.setExecutionModel(executionModel);
  KJ_IF_SOME(name, scriptName) {
    builder.setScriptName(name);
  }
  KJ_IF_SOME(version, scriptVersion) {
    builder.setScriptVersion(*version);
  }
  KJ_IF_SOME(name, dispatchNamespace) {
    builder.setDispatchNamespace(name);
  }
  KJ_IF_SOME(tags, scriptTags) {
    auto list = builder.initScriptTags(tags.size());
    for (size_t i = 0; i < tags.size(); i++) {
      list.set(i, tags[i]);
    }
  }
  KJ_IF_SOME(e, entrypoint) {
    builder.setEntryPoint(e);
  }
  auto infoBuilder = builder.initInfo();
  KJ_SWITCH_ONEOF(info) {
    KJ_CASE_ONEOF(fetch, FetchEventInfo) {
      fetch.copyTo(infoBuilder.initFetch());
    }
    KJ_CASE_ONEOF(jsrpc, JsRpcEventInfo) {
      jsrpc.copyTo(infoBuilder.initJsrpc());
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
    KJ_CASE_ONEOF(resume, Resume) {
      resume.copyTo(infoBuilder.initResume());
    }
    KJ_CASE_ONEOF(custom, CustomEventInfo) {
      infoBuilder.initCustom();
    }
  }
}

tracing::Onset tracing::Onset::clone() {
  constexpr auto cloneInfo = [](Info& info) -> tracing::Onset::Info {
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
      KJ_CASE_ONEOF(resume, Resume) {
        return resume.clone();
      }
      KJ_CASE_ONEOF(custom, CustomEventInfo) {
        return CustomEventInfo();
      }
    }
    KJ_UNREACHABLE;
  };
  return Onset(cloneInfo(info), executionModel,
      scriptName.map([](auto& name) { return kj::str(name); }),
      scriptVersion.map([](auto& version) { return capnp::clone(*version); }),
      dispatchNamespace.map([](auto& ns) { return kj::str(ns); }),
      scriptTags.map([](auto& tags) { return KJ_MAP(tag, tags) { return kj::str(tag); }; }),
      entrypoint.map([](auto& e) { return kj::str(e); }));
}

tracing::Outcome::Outcome(EventOutcome outcome, kj::Duration cpuTime, kj::Duration wallTime)
    : outcome(outcome),
      cpuTime(cpuTime),
      wallTime(wallTime) {}

tracing::Outcome::Outcome(rpc::Trace::Outcome::Reader reader)
    : outcome(reader.getOutcome()),
      cpuTime(reader.getCpuTime() * kj::MILLISECONDS),
      wallTime(reader.getWallTime() * kj::MILLISECONDS) {}

void tracing::Outcome::copyTo(rpc::Trace::Outcome::Builder builder) {
  builder.setOutcome(outcome);
  builder.setCpuTime(cpuTime / kj::MILLISECONDS);
  builder.setWallTime(wallTime / kj::MILLISECONDS);
}

tracing::Outcome tracing::Outcome::clone() {
  return Outcome(outcome, cpuTime, wallTime);
}

namespace {
tracing::TailEvent::Context getContextFromSpan(const tracing::InvocationSpanContext& context) {
  return tracing::TailEvent::Context(
      context.getTraceId(), context.getInvocationId(), context.getSpanId());
}

kj::Maybe<tracing::TailEvent::Context> getParentContextFromSpan(
    const tracing::InvocationSpanContext& context) {
  return context.getParent().map(
      [](const tracing::InvocationSpanContext& context) { return getContextFromSpan(context); });
}
}  // namespace

tracing::TailEvent::Context::Context(TraceId traceId, TraceId invocationId, SpanId spanId)
    : traceId(kj::mv(traceId)),
      invocationId(kj::mv(invocationId)),
      spanId(kj::mv(spanId)) {}

tracing::TailEvent::Context::Context(rpc::InvocationSpanContext::Reader reader)
    : traceId(TraceId::fromCapnp(reader.getTraceId())),
      invocationId(TraceId::fromCapnp(reader.getInvocationId())),
      spanId(reader.getSpanId()) {}

void tracing::TailEvent::Context::copyTo(rpc::InvocationSpanContext::Builder builder) {
  traceId.toCapnp(builder.initTraceId());
  invocationId.toCapnp(builder.initInvocationId());
  builder.setSpanId(spanId);
}

tracing::TailEvent::Context tracing::TailEvent::Context::clone() {
  return Context(traceId, invocationId, spanId);
}

tracing::TailEvent::TailEvent(const tracing::InvocationSpanContext& context,
    kj::Date timestamp,
    kj::uint sequence,
    Event&& event)
    : context(getContextFromSpan(context)),
      parentContext(getParentContextFromSpan(context)),
      timestamp(timestamp),
      sequence(sequence),
      event(kj::mv(event)) {}

tracing::TailEvent::TailEvent(Context context,
    kj::Maybe<Context> parentContext,
    kj::Date timestamp,
    kj::uint sequence,
    Event&& event)
    : context(kj::mv(context)),
      parentContext(kj::mv(parentContext)),
      timestamp(timestamp),
      sequence(sequence),
      event(kj::mv(event)) {}

namespace {
tracing::TailEvent::Context readContextFromTailEvent(const rpc::Trace::TailEvent::Reader& reader) {
  return tracing::TailEvent::Context(reader.getContext());
}

kj::Maybe<tracing::TailEvent::Context> readParentContextFromTailEvent(
    const rpc::Trace::TailEvent::Reader& reader) {
  if (!reader.hasParentContext()) return kj::none;
  return tracing::TailEvent::Context(reader.getParentContext());
}

tracing::TailEvent::Event readEventFromTailEvent(const rpc::Trace::TailEvent::Reader& reader) {
  const auto event = reader.getEvent();
  switch (event.which()) {
    case rpc::Trace::TailEvent::Event::ONSET:
      return tracing::Onset(event.getOnset());
    case rpc::Trace::TailEvent::Event::OUTCOME:
      return tracing::Outcome(event.getOutcome());
    case rpc::Trace::TailEvent::Event::HIBERNATE:
      return tracing::Hibernate(event.getHibernate());
    case rpc::Trace::TailEvent::Event::SPAN_OPEN:
      return tracing::SpanOpen(event.getSpanOpen());
    case rpc::Trace::TailEvent::Event::SPAN_CLOSE:
      return tracing::SpanClose(event.getSpanClose());
    case rpc::Trace::TailEvent::Event::ATTRIBUTE:
      return tracing::Mark(tracing::Attribute(event.getAttribute()));
    case rpc::Trace::TailEvent::Event::RETURN:
      return tracing::Mark(tracing::Return(event.getReturn()));
    case rpc::Trace::TailEvent::Event::DIAGNOSTIC_CHANNEL_EVENT:
      return tracing::Mark(tracing::DiagnosticChannelEvent(event.getDiagnosticChannelEvent()));
    case rpc::Trace::TailEvent::Event::EXCEPTION:
      return tracing::Mark(tracing::Exception(event.getException()));
    case rpc::Trace::TailEvent::Event::LOG:
      return tracing::Mark(tracing::Log(event.getLog()));
  }
  KJ_UNREACHABLE;
}
}  // namespace

tracing::TailEvent::TailEvent(rpc::Trace::TailEvent::Reader reader)
    : context(readContextFromTailEvent(reader)),
      parentContext(readParentContextFromTailEvent(reader)),
      timestamp(kj::UNIX_EPOCH + reader.getTimestampNs() * kj::NANOSECONDS),
      sequence(reader.getSequence()),
      event(readEventFromTailEvent(reader)) {}

void tracing::TailEvent::copyTo(rpc::Trace::TailEvent::Builder builder) {
  context.copyTo(builder.initContext());
  KJ_IF_SOME(parent, parentContext) {
    parent.copyTo(builder.initParentContext());
  }
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
    KJ_CASE_ONEOF(hibernate, Hibernate) {
      hibernate.copyTo(eventBuilder.initHibernate());
    }
    KJ_CASE_ONEOF(open, SpanOpen) {
      open.copyTo(eventBuilder.initSpanOpen());
    }
    KJ_CASE_ONEOF(close, SpanClose) {
      close.copyTo(eventBuilder.initSpanClose());
    }
    KJ_CASE_ONEOF(mark, Mark) {
      KJ_SWITCH_ONEOF(mark) {
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
        KJ_CASE_ONEOF(attr, Attribute) {
          attr.copyTo(eventBuilder.initAttribute());
        }
      }
    }
  }
}

tracing::TailEvent tracing::TailEvent::clone() {
  constexpr auto cloneEvent = [](Event& event) -> Event {
    KJ_SWITCH_ONEOF(event) {
      KJ_CASE_ONEOF(onset, Onset) {
        return onset.clone();
      }
      KJ_CASE_ONEOF(outcome, Outcome) {
        return outcome.clone();
      }
      KJ_CASE_ONEOF(hibernate, Hibernate) {
        return hibernate.clone();
      }
      KJ_CASE_ONEOF(open, SpanOpen) {
        return open.clone();
      }
      KJ_CASE_ONEOF(close, SpanClose) {
        return close.clone();
      }
      KJ_CASE_ONEOF(mark, Mark) {
        KJ_SWITCH_ONEOF(mark) {
          KJ_CASE_ONEOF(diag, DiagnosticChannelEvent) {
            return Mark(diag.clone());
          }
          KJ_CASE_ONEOF(ex, Exception) {
            return Mark(ex.clone());
          }
          KJ_CASE_ONEOF(log, Log) {
            return Mark(log.clone());
          }
          KJ_CASE_ONEOF(ret, Return) {
            return Mark(ret.clone());
          }
          KJ_CASE_ONEOF(attr, Attribute) {
            return Mark(attr.clone());
          }
        }
      }
    }
    KJ_UNREACHABLE;
  };
  return TailEvent(context.clone(),
      parentContext.map([](Context& parent) { return parent.clone(); }), timestamp, sequence,
      cloneEvent(event));
}

// ======================================================================================

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

void SpanBuilder::setTag(kj::ConstString key, TagValue value) {
  KJ_IF_SOME(s, span) {
    auto keyPtr = key.asPtr();
    s.tags.upsert(
        kj::mv(key), kj::mv(value), [keyPtr](TagValue& existingValue, TagValue&& newValue) {
      // This is a programming error, but not a serious one. We could alternatively just emit
      // duplicate tags and leave the Jaeger UI in charge of warning about them.
      [[maybe_unused]] static auto logged = [keyPtr]() {
        KJ_LOG(WARNING, "overwriting previous tag", keyPtr);
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

PipelineTracer::~PipelineTracer() noexcept(false) {
  KJ_IF_SOME(f, completeFulfiller) {
    f.get()->fulfill(traces.releaseAsArray());
  }
}

void PipelineTracer::addTracesFromChild(kj::ArrayPtr<kj::Own<Trace>> traces) {
  for (auto& t: traces) {
    this->traces.add(kj::addRef(*t));
  }
}

kj::Promise<kj::Array<kj::Own<Trace>>> PipelineTracer::onComplete() {
  KJ_REQUIRE(completeFulfiller == kj::none, "onComplete() can only be called once");

  auto paf = kj::newPromiseAndFulfiller<kj::Array<kj::Own<Trace>>>();
  completeFulfiller = kj::mv(paf.fulfiller);
  return kj::mv(paf.promise);
}

kj::Own<WorkerTracer> PipelineTracer::makeWorkerTracer(PipelineLogLevel pipelineLogLevel,
    ExecutionModel executionModel,
    kj::Maybe<kj::String> scriptId,
    kj::Maybe<kj::String> stableId,
    kj::Maybe<kj::String> scriptName,
    kj::Maybe<kj::Own<ScriptVersion::Reader>> scriptVersion,
    kj::Maybe<kj::String> dispatchNamespace,
    kj::Array<kj::String> scriptTags,
    kj::Maybe<kj::String> entrypoint) {
  auto trace = kj::refcounted<Trace>(kj::mv(stableId), kj::mv(scriptName), kj::mv(scriptVersion),
      kj::mv(dispatchNamespace), kj::mv(scriptId), kj::mv(scriptTags), kj::mv(entrypoint),
      executionModel);
  traces.add(kj::addRef(*trace));
  return kj::refcounted<WorkerTracer>(addRefToThis(), kj::mv(trace), pipelineLogLevel);
}

void PipelineTracer::addTrace(rpc::Trace::Reader reader) {
  traces.add(kj::refcounted<Trace>(reader));
}

WorkerTracer::WorkerTracer(
    kj::Rc<PipelineTracer> parentPipeline, kj::Own<Trace> trace, PipelineLogLevel pipelineLogLevel)
    : pipelineLogLevel(pipelineLogLevel),
      trace(kj::mv(trace)),
      parentPipeline(kj::mv(parentPipeline)),
      self(kj::refcounted<WeakRef<WorkerTracer>>(kj::Badge<WorkerTracer>{}, *this)) {}
WorkerTracer::WorkerTracer(PipelineLogLevel pipelineLogLevel, ExecutionModel executionModel)
    : pipelineLogLevel(pipelineLogLevel),
      trace(kj::refcounted<Trace>(
          kj::none, kj::none, kj::none, kj::none, kj::none, nullptr, kj::none, executionModel)),
      self(kj::refcounted<WeakRef<WorkerTracer>>(kj::Badge<WorkerTracer>{}, *this)) {}

void WorkerTracer::addLog(kj::Date timestamp, LogLevel logLevel, kj::String message, bool isSpan) {
  if (trace->exceededLogLimit) {
    return;
  }
  if (pipelineLogLevel == PipelineLogLevel::NONE) {
    return;
  }
  size_t newSize = trace->bytesUsed + sizeof(tracing::Log) + message.size();
  if (newSize > MAX_TRACE_BYTES) {
    trace->exceededLogLimit = true;
    trace->truncated = true;
    // We use a JSON encoded array/string to match other console.log() recordings:
    trace->logs.add(timestamp, LogLevel::WARN,
        kj::str(
            "[\"Log size limit exceeded: More than 128KB of data (across console.log statements, exception, request metadata and headers) was logged during a single request. Subsequent data for this request will not be recorded in logs, appear when tailing this Worker's logs, or in Tail Workers.\"]"));
    return;
  }
  trace->bytesUsed = newSize;
  if (isSpan) {
    trace->spans.add(timestamp, logLevel, kj::mv(message));
    trace->numSpans++;
    return;
  }
  trace->logs.add(timestamp, logLevel, kj::mv(message));
}

void WorkerTracer::addSpan(const Span& span, kj::String spanContext) {
  // This is where we'll actually encode the span for now.
  // Drop any spans beyond MAX_USER_SPANS.
  if (trace->numSpans >= MAX_USER_SPANS) {
    return;
  }
  if (isPredictableModeForTest()) {
    // Do not emit span duration information in predictable mode.
    addLog(span.endTime, LogLevel::LOG, kj::str("[\"span: ", span.operationName, "\"]"), true);
  } else {
    // Time since Unix epoch in seconds, with millisecond precision
    double epochSecondsStart = (span.startTime - kj::UNIX_EPOCH) / kj::MILLISECONDS / 1000.0;
    double epochSecondsEnd = (span.endTime - kj::UNIX_EPOCH) / kj::MILLISECONDS / 1000.0;
    auto message = kj::str("[\"span: ", span.operationName, " ", kj::mv(spanContext), " ",
        epochSecondsStart, " ", epochSecondsEnd, "\"]");
    addLog(span.endTime, LogLevel::LOG, kj::mv(message), true);
  }

  // TODO(cleanup): Create a function in kj::OneOf to automatically convert to a given type (i.e
  // String) to avoid having to handle each type explicitly here.
  for (const Span::TagMap::Entry& tag: span.tags) {
    auto value = [&]() {
      KJ_SWITCH_ONEOF(tag.value) {
        KJ_CASE_ONEOF(str, kj::String) {
          return kj::str(str);
        }
        KJ_CASE_ONEOF(val, int64_t) {
          return kj::str(val);
        }
        KJ_CASE_ONEOF(val, double) {
          return kj::str(val);
        }
        KJ_CASE_ONEOF(val, bool) {
          return kj::str(val);
        }
      }
      KJ_UNREACHABLE;
    }();
    kj::String message = kj::str("[\"tag: "_kj, tag.key, " => "_kj, value, "\"]");
    addLog(span.endTime, LogLevel::LOG, kj::mv(message), true);
  }
}

void WorkerTracer::addException(
    kj::Date timestamp, kj::String name, kj::String message, kj::Maybe<kj::String> stack) {
  if (trace->exceededExceptionLimit) {
    return;
  }
  // TODO(someday): For now, we're using logLevel == none as a hint to avoid doing anything
  //   expensive while tracing.  We may eventually want separate configuration for exceptions vs.
  //   logs.
  if (pipelineLogLevel == PipelineLogLevel::NONE) {
    return;
  }
  size_t newSize = trace->bytesUsed + sizeof(tracing::Exception) + name.size() + message.size();
  KJ_IF_SOME(s, stack) {
    newSize += s.size();
  }
  if (newSize > MAX_TRACE_BYTES) {
    trace->exceededExceptionLimit = true;
    trace->truncated = true;
    trace->exceptions.add(timestamp, kj::str("Error"),
        kj::str("Trace resource limit exceeded; subsequent exceptions not recorded."), kj::none);
    return;
  }
  trace->bytesUsed = newSize;
  trace->exceptions.add(timestamp, kj::mv(name), kj::mv(message), kj::mv(stack));
}

void WorkerTracer::addDiagnosticChannelEvent(
    kj::Date timestamp, kj::String channel, kj::Array<kj::byte> message) {
  if (trace->exceededDiagnosticChannelEventLimit) {
    return;
  }
  if (pipelineLogLevel == PipelineLogLevel::NONE) {
    return;
  }
  size_t newSize =
      trace->bytesUsed + sizeof(tracing::DiagnosticChannelEvent) + channel.size() + message.size();
  if (newSize > MAX_TRACE_BYTES) {
    trace->exceededDiagnosticChannelEventLimit = true;
    trace->truncated = true;
    trace->diagnosticChannelEvents.add(
        timestamp, kj::str("workerd.LimitExceeded"), kj::Array<kj::byte>());
    return;
  }
  trace->bytesUsed = newSize;
  trace->diagnosticChannelEvents.add(timestamp, kj::mv(channel), kj::mv(message));
}

void WorkerTracer::setEventInfo(kj::Date timestamp, tracing::EventInfo&& info) {
  KJ_ASSERT(trace->eventInfo == kj::none, "tracer can only be used for a single event");

  // TODO(someday): For now, we're using logLevel == none as a hint to avoid doing anything
  //   expensive while tracing.  We may eventually want separate configuration for event info vs.
  //   logs.
  // TODO(perf): Find a way to allow caller to avoid the cost of generation if the info struct
  //   won't be used?
  if (pipelineLogLevel == PipelineLogLevel::NONE) {
    return;
  }

  trace->eventTimestamp = timestamp;

  size_t newSize = trace->bytesUsed;
  KJ_SWITCH_ONEOF(info) {
    KJ_CASE_ONEOF(fetch, tracing::FetchEventInfo) {
      newSize += fetch.url.size();
      for (const auto& header: fetch.headers) {
        newSize += header.name.size() + header.value.size();
      }
      newSize += fetch.cfJson.size();
      if (newSize > MAX_TRACE_BYTES) {
        trace->truncated = true;
        trace->logs.add(timestamp, LogLevel::WARN,
            kj::str("[\"Trace resource limit exceeded; could not capture event info.\"]"));
        trace->eventInfo = tracing::FetchEventInfo(fetch.method, {}, {}, {});
        return;
      }
    }
    KJ_CASE_ONEOF_DEFAULT {}
  }
  trace->bytesUsed = newSize;
  trace->eventInfo = kj::mv(info);
}

void WorkerTracer::setOutcome(EventOutcome outcome, kj::Duration cpuTime, kj::Duration wallTime) {
  trace->outcome = outcome;
  trace->cpuTime = cpuTime;
  trace->wallTime = wallTime;
}

void WorkerTracer::setFetchResponseInfo(tracing::FetchResponseInfo&& info) {
  // Match the behavior of setEventInfo(). Any resolution of the TODO comments
  // in setEventInfo() that are related to this check while probably also affect
  // this function.
  if (pipelineLogLevel == PipelineLogLevel::NONE) {
    return;
  }

  KJ_REQUIRE(KJ_REQUIRE_NONNULL(trace->eventInfo).is<tracing::FetchEventInfo>());
  KJ_ASSERT(trace->fetchResponseInfo == kj::none, "setFetchResponseInfo can only be called once");
  trace->fetchResponseInfo = kj::mv(info);
}

void WorkerTracer::extractTrace(rpc::Trace::Builder builder) {
  trace->copyTo(builder);
}

void WorkerTracer::setTrace(rpc::Trace::Reader reader) {
  trace->mergeFrom(reader, pipelineLogLevel);
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
