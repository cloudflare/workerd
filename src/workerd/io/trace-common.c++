#include "trace-common.h"

#include "trace-legacy.h"

namespace workerd::trace {

// ======================================================================================
// FetchEventInfo

namespace {
static kj::HttpMethod validateMethod(capnp::HttpMethod method) {
  KJ_REQUIRE(method <= capnp::HttpMethod::UNSUBSCRIBE, "unknown method", method);
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
  kj::Vector<Header> v;
  v.addAll(reader.getHeaders());
  headers = v.releaseAsArray();
}

void FetchEventInfo::copyTo(rpc::Trace::FetchEventInfo::Builder builder) const {
  builder.setMethod(static_cast<capnp::HttpMethod>(method));
  builder.setUrl(url);
  builder.setCfJson(cfJson);

  auto list = builder.initHeaders(headers.size());
  for (auto i: kj::indices(headers)) {
    headers[i].copyTo(list[i]);
  }
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

// ======================================================================================
// FetchResponseInfo
FetchResponseInfo::FetchResponseInfo(uint16_t statusCode): statusCode(statusCode) {}

FetchResponseInfo::FetchResponseInfo(rpc::Trace::FetchResponseInfo::Reader reader)
    : statusCode(reader.getStatusCode()) {}

void FetchResponseInfo::copyTo(rpc::Trace::FetchResponseInfo::Builder builder) {
  builder.setStatusCode(statusCode);
}

// ======================================================================================
// JsRpcEventInfo

JsRpcEventInfo::JsRpcEventInfo(kj::String methodName): methodName(kj::mv(methodName)) {}

JsRpcEventInfo::JsRpcEventInfo(rpc::Trace::JsRpcEventInfo::Reader reader)
    : methodName(kj::str(reader.getMethodName())) {}

void JsRpcEventInfo::copyTo(rpc::Trace::JsRpcEventInfo::Builder builder) {
  builder.setMethodName(methodName);
}

// ======================================================================================
// ScheduledEventInfo

ScheduledEventInfo::ScheduledEventInfo(double scheduledTime, kj::String cron)
    : scheduledTime(scheduledTime),
      cron(kj::mv(cron)) {}

ScheduledEventInfo::ScheduledEventInfo(rpc::Trace::ScheduledEventInfo::Reader reader)
    : scheduledTime(reader.getScheduledTime()),
      cron(kj::str(reader.getCron())) {}

void ScheduledEventInfo::copyTo(rpc::Trace::ScheduledEventInfo::Builder builder) {
  builder.setScheduledTime(scheduledTime);
  builder.setCron(cron);
}

// ======================================================================================
// AlarmEventInfo

AlarmEventInfo::AlarmEventInfo(kj::Date scheduledTime): scheduledTime(scheduledTime) {}

AlarmEventInfo::AlarmEventInfo(rpc::Trace::AlarmEventInfo::Reader reader)
    : scheduledTime(reader.getScheduledTimeMs() * kj::MILLISECONDS + kj::UNIX_EPOCH) {}

void AlarmEventInfo::copyTo(rpc::Trace::AlarmEventInfo::Builder builder) {
  builder.setScheduledTimeMs((scheduledTime - kj::UNIX_EPOCH) / kj::MILLISECONDS);
}

// ======================================================================================
// QueueEventInfo

QueueEventInfo::QueueEventInfo(kj::String queueName, uint32_t batchSize)
    : queueName(kj::mv(queueName)),
      batchSize(batchSize) {}

QueueEventInfo::QueueEventInfo(rpc::Trace::QueueEventInfo::Reader reader)
    : queueName(kj::heapString(reader.getQueueName())),
      batchSize(reader.getBatchSize()) {}

void QueueEventInfo::copyTo(rpc::Trace::QueueEventInfo::Builder builder) {
  builder.setQueueName(queueName);
  builder.setBatchSize(batchSize);
}

// ======================================================================================
// EmailEventInfo

EmailEventInfo::EmailEventInfo(kj::String mailFrom, kj::String rcptTo, uint32_t rawSize)
    : mailFrom(kj::mv(mailFrom)),
      rcptTo(kj::mv(rcptTo)),
      rawSize(rawSize) {}

EmailEventInfo::EmailEventInfo(rpc::Trace::EmailEventInfo::Reader reader)
    : mailFrom(kj::heapString(reader.getMailFrom())),
      rcptTo(kj::heapString(reader.getRcptTo())),
      rawSize(reader.getRawSize()) {}

void EmailEventInfo::copyTo(rpc::Trace::EmailEventInfo::Builder builder) {
  builder.setMailFrom(mailFrom);
  builder.setRcptTo(rcptTo);
  builder.setRawSize(rawSize);
}

// ======================================================================================
// HibernatableWebSocketEventInfo

HibernatableWebSocketEventInfo::HibernatableWebSocketEventInfo(Type type): type(type) {}

HibernatableWebSocketEventInfo::HibernatableWebSocketEventInfo(
    rpc::Trace::HibernatableWebSocketEventInfo::Reader reader)
    : type(readFrom(reader)) {}

void HibernatableWebSocketEventInfo::copyTo(
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

// ======================================================================================
// TraceEventInfo

namespace {
kj::Vector<TraceEventInfo::TraceItem> getTraceItemsFromTraces(kj::ArrayPtr<kj::Own<Trace>> traces) {
  return KJ_MAP(t, traces) -> TraceEventInfo::TraceItem {
    return TraceEventInfo::TraceItem(
        t->onsetInfo.scriptName.map([](auto& scriptName) { return kj::str(scriptName); }));
  };
}

kj::Vector<TraceEventInfo::TraceItem> getTraceItemsFromReader(
    rpc::Trace::TraceEventInfo::Reader reader) {
  return KJ_MAP(r, reader.getTraces()) -> Trace::TraceEventInfo::TraceItem {
    return Trace::TraceEventInfo::TraceItem(r);
  };
}
}  // namespace

TraceEventInfo::TraceEventInfo(kj::ArrayPtr<kj::Own<Trace>> traces)
    : traces(getTraceItemsFromTraces(traces)) {}

TraceEventInfo::TraceEventInfo(rpc::Trace::TraceEventInfo::Reader reader)
    : traces(getTraceItemsFromReader(reader)) {}

void TraceEventInfo::copyTo(rpc::Trace::TraceEventInfo::Builder builder) {
  auto list = builder.initTraces(traces.size());
  for (auto i: kj::indices(traces)) {
    traces[i].copyTo(list[i]);
  }
}

TraceEventInfo::TraceItem::TraceItem(kj::Maybe<kj::String> scriptName)
    : scriptName(kj::mv(scriptName)) {}

TraceEventInfo::TraceItem::TraceItem(rpc::Trace::TraceEventInfo::TraceItem::Reader reader)
    : scriptName(kj::str(reader.getScriptName())) {}

void TraceEventInfo::TraceItem::copyTo(rpc::Trace::TraceEventInfo::TraceItem::Builder builder) {
  KJ_IF_SOME(name, scriptName) {
    builder.setScriptName(name);
  }
}

}  // namespace workerd::trace
