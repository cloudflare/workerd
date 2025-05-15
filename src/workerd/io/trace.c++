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

void tracing::FetchEventInfo::copyTo(rpc::Trace::FetchEventInfo::Builder builder) const {
  builder.setMethod(static_cast<capnp::HttpMethod>(method));
  builder.setUrl(url);
  builder.setCfJson(cfJson);

  auto list = builder.initHeaders(headers.size());
  for (auto i: kj::indices(headers)) {
    headers[i].copyTo(list[i]);
  }
}

tracing::FetchEventInfo tracing::FetchEventInfo::clone() const {
  return FetchEventInfo(
      method, kj::str(url), kj::str(cfJson), KJ_MAP(h, headers) { return h.clone(); });
}

tracing::FetchEventInfo::Header::Header(kj::String name, kj::String value)
    : name(kj::mv(name)),
      value(kj::mv(value)) {}

tracing::FetchEventInfo::Header::Header(rpc::Trace::FetchEventInfo::Header::Reader reader)
    : name(kj::str(reader.getName())),
      value(kj::str(reader.getValue())) {}

void tracing::FetchEventInfo::Header::copyTo(
    rpc::Trace::FetchEventInfo::Header::Builder builder) const {
  builder.setName(name);
  builder.setValue(value);
}

tracing::FetchEventInfo::Header tracing::FetchEventInfo::Header::clone() const {
  return Header(kj::str(name), kj::str(value));
}

tracing::JsRpcEventInfo::JsRpcEventInfo(kj::String methodName): methodName(kj::mv(methodName)) {}

tracing::JsRpcEventInfo::JsRpcEventInfo(rpc::Trace::JsRpcEventInfo::Reader reader)
    : methodName(kj::str(reader.getMethodName())) {}

void tracing::JsRpcEventInfo::copyTo(rpc::Trace::JsRpcEventInfo::Builder builder) const {
  builder.setMethodName(methodName);
}

tracing::JsRpcEventInfo tracing::JsRpcEventInfo::clone() const {
  return JsRpcEventInfo(kj::str(methodName));
}

tracing::ScheduledEventInfo::ScheduledEventInfo(double scheduledTime, kj::String cron)
    : scheduledTime(scheduledTime),
      cron(kj::mv(cron)) {}

tracing::ScheduledEventInfo::ScheduledEventInfo(rpc::Trace::ScheduledEventInfo::Reader reader)
    : scheduledTime(reader.getScheduledTime()),
      cron(kj::str(reader.getCron())) {}

void tracing::ScheduledEventInfo::copyTo(rpc::Trace::ScheduledEventInfo::Builder builder) const {
  builder.setScheduledTime(scheduledTime);
  builder.setCron(cron);
}

tracing::ScheduledEventInfo tracing::ScheduledEventInfo::clone() const {
  return ScheduledEventInfo(scheduledTime, kj::str(cron));
}

tracing::AlarmEventInfo::AlarmEventInfo(kj::Date scheduledTime): scheduledTime(scheduledTime) {}

tracing::AlarmEventInfo::AlarmEventInfo(rpc::Trace::AlarmEventInfo::Reader reader)
    : scheduledTime(reader.getScheduledTimeMs() * kj::MILLISECONDS + kj::UNIX_EPOCH) {}

void tracing::AlarmEventInfo::copyTo(rpc::Trace::AlarmEventInfo::Builder builder) const {
  builder.setScheduledTimeMs((scheduledTime - kj::UNIX_EPOCH) / kj::MILLISECONDS);
}

tracing::AlarmEventInfo tracing::AlarmEventInfo::clone() const {
  return AlarmEventInfo(scheduledTime);
}

tracing::QueueEventInfo::QueueEventInfo(kj::String queueName, uint32_t batchSize)
    : queueName(kj::mv(queueName)),
      batchSize(batchSize) {}

tracing::QueueEventInfo::QueueEventInfo(rpc::Trace::QueueEventInfo::Reader reader)
    : queueName(kj::heapString(reader.getQueueName())),
      batchSize(reader.getBatchSize()) {}

void tracing::QueueEventInfo::copyTo(rpc::Trace::QueueEventInfo::Builder builder) const {
  builder.setQueueName(queueName);
  builder.setBatchSize(batchSize);
}

tracing::QueueEventInfo tracing::QueueEventInfo::clone() const {
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

void tracing::EmailEventInfo::copyTo(rpc::Trace::EmailEventInfo::Builder builder) const {
  builder.setMailFrom(mailFrom);
  builder.setRcptTo(rcptTo);
  builder.setRawSize(rawSize);
}

tracing::EmailEventInfo tracing::EmailEventInfo::clone() const {
  return EmailEventInfo(kj::str(mailFrom), kj::str(rcptTo), rawSize);
}

namespace {
kj::Vector<tracing::TraceEventInfo::TraceItem> getTraceItemsFromTraces(
    kj::ArrayPtr<kj::Own<Trace>> traces) {
  return KJ_MAP(t, traces) -> tracing::TraceEventInfo::TraceItem {
    return tracing::TraceEventInfo::TraceItem(
        t->scriptName.map([](auto& scriptName) { return kj::str(scriptName); }));
  };
}

kj::Vector<tracing::TraceEventInfo::TraceItem> getTraceItemsFromReader(
    rpc::Trace::TraceEventInfo::Reader reader) {
  return KJ_MAP(r, reader.getTraces()) -> tracing::TraceEventInfo::TraceItem {
    return tracing::TraceEventInfo::TraceItem(r);
  };
}
}  // namespace

tracing::TraceEventInfo::TraceEventInfo(kj::ArrayPtr<kj::Own<Trace>> traces)
    : traces(getTraceItemsFromTraces(traces)) {}

tracing::TraceEventInfo::TraceEventInfo(rpc::Trace::TraceEventInfo::Reader reader)
    : traces(getTraceItemsFromReader(reader)) {}

void tracing::TraceEventInfo::copyTo(rpc::Trace::TraceEventInfo::Builder builder) const {
  auto list = builder.initTraces(traces.size());
  for (auto i: kj::indices(traces)) {
    traces[i].copyTo(list[i]);
  }
}

tracing::TraceEventInfo tracing::TraceEventInfo::clone() const {
  return TraceEventInfo(KJ_MAP(item, traces) { return item.clone(); });
}

tracing::TraceEventInfo::TraceItem::TraceItem(kj::Maybe<kj::String> scriptName)
    : scriptName(kj::mv(scriptName)) {}

tracing::TraceEventInfo::TraceItem::TraceItem(rpc::Trace::TraceEventInfo::TraceItem::Reader reader)
    : scriptName(kj::str(reader.getScriptName())) {}

void tracing::TraceEventInfo::TraceItem::copyTo(
    rpc::Trace::TraceEventInfo::TraceItem::Builder builder) const {
  KJ_IF_SOME(name, scriptName) {
    builder.setScriptName(name);
  }
}

tracing::TraceEventInfo::TraceItem tracing::TraceEventInfo::TraceItem::clone() const {
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

void tracing::DiagnosticChannelEvent::copyTo(
    rpc::Trace::DiagnosticChannelEvent::Builder builder) const {
  builder.setTimestampNs((timestamp - kj::UNIX_EPOCH) / kj::NANOSECONDS);
  builder.setChannel(channel);
  builder.setMessage(message);
}

tracing::DiagnosticChannelEvent tracing::DiagnosticChannelEvent::clone() const {
  return DiagnosticChannelEvent(timestamp, kj::str(channel), kj::heapArray<kj::byte>(message));
}

tracing::HibernatableWebSocketEventInfo::HibernatableWebSocketEventInfo(Type type): type(type) {}

tracing::HibernatableWebSocketEventInfo::HibernatableWebSocketEventInfo(
    rpc::Trace::HibernatableWebSocketEventInfo::Reader reader)
    : type(readFrom(reader)) {}

void tracing::HibernatableWebSocketEventInfo::copyTo(
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

tracing::HibernatableWebSocketEventInfo tracing::HibernatableWebSocketEventInfo::clone() const {
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

void tracing::FetchResponseInfo::copyTo(rpc::Trace::FetchResponseInfo::Builder builder) const {
  builder.setStatusCode(statusCode);
}

tracing::FetchResponseInfo tracing::FetchResponseInfo::clone() const {
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

void Trace::copyTo(rpc::Trace::Builder builder) const {
  {
    auto list = builder.initLogs(logs.size());
    for (auto i: kj::indices(logs)) {
      logs[i].copyTo(list[i]);
    }
  }

  {
    // Add spans to the builder.
    auto list = builder.initSpans(spans.size());
    for (auto i: kj::indices(spans)) {
      spans[i].copyTo(list[i]);
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

void tracing::Log::copyTo(rpc::Trace::Log::Builder builder) const {
  builder.setTimestampNs((timestamp - kj::UNIX_EPOCH) / kj::NANOSECONDS);
  builder.setLogLevel(logLevel);
  builder.setMessage(message);
}

tracing::Log tracing::Log::clone() const {
  return Log(timestamp, logLevel, kj::str(message));
}

void tracing::Exception::copyTo(rpc::Trace::Exception::Builder builder) const {
  builder.setTimestampNs((timestamp - kj::UNIX_EPOCH) / kj::NANOSECONDS);
  builder.setName(name);
  builder.setMessage(message);
  KJ_IF_SOME(s, stack) {
    builder.setStack(s);
  }
}

tracing::Exception tracing::Exception::clone() const {
  return Exception(timestamp, kj::str(name), kj::str(message),
      stack.map([](auto& stack) { return kj::str(stack); }));
}

void Trace::mergeFrom(rpc::Trace::Reader reader, PipelineLogLevel pipelineLogLevel) {
  // Sandboxed workers currently record their traces as if the pipeline log level were set to
  // "full", so we may need to filter out the extra data after receiving the traces back.
  if (pipelineLogLevel != PipelineLogLevel::NONE) {
    logs.addAll(reader.getLogs());
    spans.addAll(reader.getSpans());
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

void tracing::Resume::copyTo(rpc::Trace::Resume::Builder builder) const {
  KJ_IF_SOME(attach, attachment) {
    builder.setAttachment(attach);
  }
}

tracing::Resume tracing::Resume::clone() const {
  return Resume(attachment.map([](auto& attach) { return kj::heapArray<kj::byte>(attach); }));
}

tracing::Hibernate::Hibernate() {}

tracing::Hibernate::Hibernate(rpc::Trace::Hibernate::Reader reader) {}

void tracing::Hibernate::copyTo(rpc::Trace::Hibernate::Builder builder) const {}

tracing::Hibernate tracing::Hibernate::clone() const {
  return Hibernate();
}

tracing::Attribute::Attribute(kj::String name, Value&& value)
    : name(kj::mv(name)),
      value(kj::arr(kj::mv(value))) {}

tracing::Attribute::Attribute(kj::String name, Values&& value)
    : name(kj::mv(name)),
      value(kj::mv(value)) {}

namespace {
kj::Array<tracing::Attribute::Value> readValues(const rpc::Trace::Attribute::Reader& reader) {
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
        return static_cast<int64_t>(inner.getInt());
      }
    }
    KJ_UNREACHABLE;
  };

  // There should always be a value and it always have at least one entry in the list.
  KJ_ASSERT(reader.hasValue());
  auto value = reader.getValue();
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

void tracing::Attribute::copyTo(rpc::Trace::Attribute::Builder builder) const {
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
      KJ_CASE_ONEOF(i, int64_t) {
        builder.initInner().setInt(i);
      }
    }
  };
  builder.setName(name.asPtr());
  auto vec = builder.initValue(value.size());
  for (size_t n = 0; n < value.size(); n++) {
    writeValue(vec[n], value[n]);
  }
}

tracing::Attribute tracing::Attribute::clone() const {
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
      KJ_CASE_ONEOF(i, int64_t) {
        return i;
      }
    }
    KJ_UNREACHABLE;
  };

  return Attribute(kj::str(name), KJ_MAP(v, value) { return cloneValue(v); });
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

void tracing::Return::copyTo(rpc::Trace::Return::Builder builder) const {
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

tracing::Return tracing::Return::clone() const {
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

tracing::SpanOpen::SpanOpen(uint64_t parentSpanId, kj::String operationName, kj::Maybe<Info> info)
    : operationName(kj::mv(operationName)),
      info(kj::mv(info)),
      parentSpanId(parentSpanId) {}

namespace {
kj::Maybe<tracing::SpanOpen::Info> readSpanOpenInfo(rpc::Trace::SpanOpen::Reader& reader) {
  auto info = reader.getInfo();
  switch (info.which()) {
    case rpc::Trace::SpanOpen::Info::EMPTY:
      return kj::none;
    case rpc::Trace::SpanOpen::Info::FETCH: {
      return kj::Maybe(tracing::FetchEventInfo(info.getFetch()));
    }
    case rpc::Trace::SpanOpen::Info::JS_RPC: {
      return kj::Maybe(tracing::JsRpcEventInfo(info.getJsRpc()));
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
    // TODO(streaming-tail): Propagate parentSpanId properly
    : operationName(kj::str(reader.getOperationName())),
      info(readSpanOpenInfo(reader)),
      parentSpanId(0) {}

void tracing::SpanOpen::copyTo(rpc::Trace::SpanOpen::Builder builder) const {
  builder.setOperationName(operationName.asPtr());
  KJ_IF_SOME(i, info) {
    auto infoBuilder = builder.initInfo();
    KJ_SWITCH_ONEOF(i) {
      KJ_CASE_ONEOF(fetch, tracing::FetchEventInfo) {
        fetch.copyTo(infoBuilder.initFetch());
      }
      KJ_CASE_ONEOF(jsrpc, tracing::JsRpcEventInfo) {
        jsrpc.copyTo(infoBuilder.initJsRpc());
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

tracing::SpanOpen tracing::SpanOpen::clone() const {
  constexpr auto cloneInfo = [](const kj::Maybe<Info>& info) -> kj::Maybe<tracing::SpanOpen::Info> {
    return info.map([](const Info& info) -> tracing::SpanOpen::Info {
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
  return SpanOpen(parentSpanId, kj::str(operationName), cloneInfo(info));
}

tracing::SpanClose::SpanClose(EventOutcome outcome): outcome(outcome) {}

tracing::SpanClose::SpanClose(rpc::Trace::SpanClose::Reader reader): outcome(reader.getOutcome()) {}

void tracing::SpanClose::copyTo(rpc::Trace::SpanClose::Builder builder) const {
  builder.setOutcome(outcome);
}

tracing::SpanClose tracing::SpanClose::clone() const {
  return SpanClose(outcome);
}

namespace {
kj::Maybe<kj::String> readLabelFromReader(const rpc::Trace::Link::Reader& reader) {
  if (!reader.hasLabel()) return kj::none;
  return kj::str(reader.getLabel());
}
tracing::TraceId readTraceIdFromReader(const rpc::Trace::Link::Reader& reader) {
  KJ_ASSERT(reader.hasContext());
  auto context = reader.getContext();
  return tracing::TraceId::fromCapnp(context.getTraceId());
}
tracing::TraceId readInvocationIdFromReader(const rpc::Trace::Link::Reader& reader) {
  KJ_ASSERT(reader.hasContext());
  auto context = reader.getContext();
  return tracing::TraceId::fromCapnp(context.getInvocationId());
}
tracing::SpanId readSpanIdFromReader(const rpc::Trace::Link::Reader& reader) {
  KJ_ASSERT(reader.hasContext());
  auto context = reader.getContext();
  return tracing::SpanId(context.getSpanId());
}
}  // namespace

tracing::Link::Link(const InvocationSpanContext& other, kj::Maybe<kj::String> label)
    : Link(kj::mv(label), other.getTraceId(), other.getInvocationId(), other.getSpanId()) {}

tracing::Link::Link(
    kj::Maybe<kj::String> label, TraceId traceId, TraceId invocationId, SpanId spanId)
    : label(kj::mv(label)),
      traceId(kj::mv(traceId)),
      invocationId(kj::mv(invocationId)),
      spanId(kj::mv(spanId)) {}

tracing::Link::Link(rpc::Trace::Link::Reader reader)
    : label(readLabelFromReader(reader)),
      traceId(readTraceIdFromReader(reader)),
      invocationId(readInvocationIdFromReader(reader)),
      spanId(readSpanIdFromReader(reader)) {}

void tracing::Link::copyTo(rpc::Trace::Link::Builder builder) const {
  KJ_IF_SOME(l, label) {
    builder.setLabel(l);
  }
  auto ctx = builder.initContext();
  traceId.toCapnp(ctx.initTraceId());
  invocationId.toCapnp(ctx.initInvocationId());
  ctx.setSpanId(spanId.getId());
}

tracing::Link tracing::Link::clone() const {
  return Link(
      label.map([](const kj::String& str) { return kj::str(str); }), traceId, invocationId, spanId);
}

tracing::Onset::Info tracing::readOnsetInfo(const rpc::Trace::Onset::Info::Reader& info) {
  switch (info.which()) {
    case rpc::Trace::Onset::Info::FETCH: {
      return tracing::FetchEventInfo(info.getFetch());
    }
    case rpc::Trace::Onset::Info::JS_RPC: {
      return tracing::JsRpcEventInfo(info.getJsRpc());
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

void tracing::writeOnsetInfo(
    const tracing::Onset::Info& info, rpc::Trace::Onset::Info::Builder& infoBuilder) {
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
    KJ_CASE_ONEOF(resume, Resume) {
      resume.copyTo(infoBuilder.initResume());
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
kj::Maybe<tracing::Onset::TriggerContext> getTriggerContextFromReader(
    const rpc::Trace::Onset::Reader& reader) {
  if (!reader.hasTrigger()) return kj::none;
  auto trigger = reader.getTrigger();
  return tracing::Onset::TriggerContext(tracing::TraceId::fromCapnp(trigger.getTraceId()),
      tracing::TraceId::fromCapnp(trigger.getInvocationId()), tracing::SpanId(trigger.getSpanId()));
}
tracing::Onset::WorkerInfo getWorkerInfoFromReader(const rpc::Trace::Onset::Reader& reader) {
  return tracing::Onset::WorkerInfo{
    .executionModel = reader.getExecutionModel(),
    .scriptName = getScriptNameFromReader(reader),
    .scriptVersion = getScriptVersionFromReader(reader),
    .dispatchNamespace = getDispatchNamespaceFromReader(reader),
    .scriptTags = getScriptTagsFromReader(reader),
    .entrypoint = getEntrypointFromReader(reader),
  };
}
}  // namespace

tracing::Onset::Onset(tracing::Onset::Info&& info,
    tracing::Onset::WorkerInfo&& workerInfo,
    kj::Maybe<TriggerContext> maybeTrigger)
    : info(kj::mv(info)),
      workerInfo(kj::mv(workerInfo)),
      trigger(kj::mv(maybeTrigger)) {}

tracing::Onset::Onset(rpc::Trace::Onset::Reader reader)
    : info(readOnsetInfo(reader.getInfo())),
      workerInfo(getWorkerInfoFromReader(reader)),
      trigger(getTriggerContextFromReader(reader)) {}

void tracing::Onset::copyTo(rpc::Trace::Onset::Builder builder) const {
  builder.setExecutionModel(workerInfo.executionModel);
  KJ_IF_SOME(name, workerInfo.scriptName) {
    builder.setScriptName(name);
  }
  KJ_IF_SOME(version, workerInfo.scriptVersion) {
    builder.setScriptVersion(*version);
  }
  KJ_IF_SOME(name, workerInfo.dispatchNamespace) {
    builder.setDispatchNamespace(name);
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
  KJ_IF_SOME(t, trigger) {
    auto ctx = builder.initTrigger();
    t.traceId.toCapnp(ctx.initTraceId());
    t.invocationId.toCapnp(ctx.getInvocationId());
    ctx.setSpanId(t.spanId.getId());
  }
  auto infoBuilder = builder.initInfo();
  writeOnsetInfo(info, infoBuilder);
}

tracing::Onset::WorkerInfo tracing::Onset::WorkerInfo::clone() const {
  return WorkerInfo{
    .executionModel = executionModel,
    .scriptName = scriptName.map([](auto& str) { return kj::str(str); }),
    .scriptVersion = scriptVersion.map([](auto& version) { return capnp::clone(*version); }),
    .dispatchNamespace = dispatchNamespace.map([](auto& str) { return kj::str(str); }),
    .scriptTags =
        scriptTags.map([](auto& tags) { return KJ_MAP(tag, tags) { return kj::str(tag); }; }),
    .entrypoint = entrypoint.map([](auto& str) { return kj::str(str); }),
  };
}

tracing::EventInfo tracing::cloneEventInfo(const tracing::EventInfo& info) {
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
}

tracing::Onset tracing::Onset::clone() const {
  return Onset(cloneEventInfo(info), workerInfo.clone(), trigger.map([](const TriggerContext& ctx) {
    return TriggerContext(ctx.traceId, ctx.invocationId, ctx.spanId);
  }));
}

tracing::Outcome::Outcome(EventOutcome outcome, kj::Duration cpuTime, kj::Duration wallTime)
    : outcome(outcome),
      cpuTime(cpuTime),
      wallTime(wallTime) {}

tracing::Outcome::Outcome(rpc::Trace::Outcome::Reader reader)
    : outcome(reader.getOutcome()),
      cpuTime(reader.getCpuTime() * kj::MILLISECONDS),
      wallTime(reader.getWallTime() * kj::MILLISECONDS) {}

void tracing::Outcome::copyTo(rpc::Trace::Outcome::Builder builder) const {
  builder.setOutcome(outcome);
  builder.setCpuTime(cpuTime / kj::MILLISECONDS);
  builder.setWallTime(wallTime / kj::MILLISECONDS);
}

tracing::Outcome tracing::Outcome::clone() const {
  return Outcome(outcome, cpuTime, wallTime);
}

tracing::TailEvent::TailEvent(const tracing::InvocationSpanContext& context,
    kj::Date timestamp,
    kj::uint sequence,
    Event&& event)
    : traceId(context.getTraceId()),
      invocationId(context.getInvocationId()),
      spanId(context.getSpanId()),
      timestamp(timestamp),
      sequence(sequence),
      event(kj::mv(event)) {}

tracing::TailEvent::TailEvent(TraceId traceId,
    TraceId invocationId,
    SpanId spanId,
    kj::Date timestamp,
    kj::uint sequence,
    Event&& event)
    : traceId(kj::mv(traceId)),
      invocationId(kj::mv(invocationId)),
      spanId(kj::mv(spanId)),
      timestamp(timestamp),
      sequence(sequence),
      event(kj::mv(event)) {}

namespace {
tracing::TailEvent::Event readEventFromTailEvent(const rpc::Trace::TailEvent::Reader& reader) {
  const auto event = reader.getEvent();
  switch (event.which()) {
    case rpc::Trace::TailEvent::Event::ONSET: {
      return tracing::Onset(event.getOnset());
    }
    case rpc::Trace::TailEvent::Event::OUTCOME: {
      return tracing::Outcome(event.getOutcome());
    }
    case rpc::Trace::TailEvent::Event::HIBERNATE: {
      return tracing::Hibernate(event.getHibernate());
    }
    case rpc::Trace::TailEvent::Event::SPAN_OPEN: {
      return tracing::SpanOpen(event.getSpanOpen());
    }
    case rpc::Trace::TailEvent::Event::SPAN_CLOSE: {
      return tracing::SpanClose(event.getSpanClose());
    }
    case rpc::Trace::TailEvent::Event::COMPLETED_SPAN: {
      return CompleteSpan(event.getCompletedSpan());
    }
    case rpc::Trace::TailEvent::Event::ATTRIBUTE: {
      auto listReader = event.getAttribute();
      kj::Vector<tracing::Attribute> attrs(listReader.size());
      for (size_t n = 0; n < listReader.size(); n++) {
        attrs.add(tracing::Attribute(listReader[n]));
      }
      return attrs.releaseAsArray();
    }
    case rpc::Trace::TailEvent::Event::RETURN: {
      return tracing::Return(event.getReturn());
    }
    case rpc::Trace::TailEvent::Event::DIAGNOSTIC_CHANNEL_EVENT: {
      return tracing::DiagnosticChannelEvent(event.getDiagnosticChannelEvent());
    }
    case rpc::Trace::TailEvent::Event::EXCEPTION: {
      return tracing::Exception(event.getException());
    }
    case rpc::Trace::TailEvent::Event::LOG: {
      return tracing::Log(event.getLog());
    }
    case rpc::Trace::TailEvent::Event::LINK: {
      return tracing::Link(event.getLink());
    }
  }
  KJ_UNREACHABLE;
}
}  // namespace

tracing::TailEvent::TailEvent(rpc::Trace::TailEvent::Reader reader)
    : traceId(TraceId::fromCapnp(reader.getContext().getTraceId())),
      invocationId(TraceId::fromCapnp(reader.getContext().getInvocationId())),
      spanId(SpanId(reader.getContext().getSpanId())),
      timestamp(kj::UNIX_EPOCH + reader.getTimestampNs() * kj::NANOSECONDS),
      sequence(reader.getSequence()),
      event(readEventFromTailEvent(reader)) {}

void tracing::TailEvent::copyTo(rpc::Trace::TailEvent::Builder builder) const {
  auto context = builder.initContext();
  traceId.toCapnp(context.initTraceId());
  invocationId.toCapnp(context.initInvocationId());
  context.setSpanId(spanId.getId());
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
    KJ_CASE_ONEOF(span, CompleteSpan) {
      span.copyTo(eventBuilder.initCompletedSpan());
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
    KJ_CASE_ONEOF(link, Link) {
      link.copyTo(eventBuilder.initLink());
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

tracing::TailEvent tracing::TailEvent::clone() const {
  constexpr auto cloneEvent = [](const Event& event) -> Event {
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
      KJ_CASE_ONEOF(span, CompleteSpan) {
        return span.clone();
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
      KJ_CASE_ONEOF(link, Link) {
        return link.clone();
      }
      KJ_CASE_ONEOF(attrs, tracing::CustomInfo) {
        return KJ_MAP(attr, attrs) { return attr.clone(); };
      }
    }
    KJ_UNREACHABLE;
  };
  return TailEvent(traceId, invocationId, spanId, timestamp, sequence, cloneEvent(event));
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

Span::TagValue spanTagClone(const Span::TagValue& tag) {
  KJ_SWITCH_ONEOF(tag) {
    KJ_CASE_ONEOF(str, kj::String) {
      return kj::str(str);
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

kj::String spanTagStr(const Span::TagValue& tag) {
  KJ_SWITCH_ONEOF(tag) {
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
    KJ_CASE_ONEOF(s, kj::String) {
      builder.setString(s);
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
      return kj::heapString(value.getString());
    default:
      KJ_UNREACHABLE;
  }
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
  CompleteSpan copy(kj::ConstString(kj::str(operationName)), startTime);
  copy.endTime = endTime;
  copy.tags.reserve(tags.size());
  for (auto& tag: tags) {
    copy.tags.insert(kj::ConstString(kj::str(tag.key)), spanTagClone(tag.value));
  }
  copy.spanId = spanId;
  copy.parentSpanId = parentSpanId;
  return copy;
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
