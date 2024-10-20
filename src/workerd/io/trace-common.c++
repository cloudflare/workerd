// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#include "trace-common.h"

#include "trace-legacy.h"

#include <capnp/message.h>
#include <capnp/schema.h>
#include <kj/compat/http.h>

namespace workerd::trace {

// ======================================================================================
// Onset

namespace {
kj::Maybe<kj::String> maybeGetScriptName(const rpc::Trace::Onset::Reader& reader) {
  if (!reader.hasScriptName()) return kj::none;
  return kj::str(reader.getScriptName());
}
kj::Maybe<kj::Own<ScriptVersion::Reader>> maybeGetScriptVersion(
    const rpc::Trace::Onset::Reader& reader) {
  if (!reader.hasScriptVersion()) return kj::none;
  return capnp::clone(reader.getScriptVersion());
}
kj::Maybe<kj::String> maybeGetDispatchNamespace(const rpc::Trace::Onset::Reader& reader) {
  if (!reader.hasDispatchNamespace()) return kj::none;
  return kj::str(reader.getDispatchNamespace());
}
kj::Maybe<kj::String> maybeGetScriptId(const rpc::Trace::Onset::Reader& reader) {
  if (!reader.hasScriptId()) return kj::none;
  return kj::str(reader.getScriptId());
}
kj::Array<kj::String> maybeGetScriptTags(const rpc::Trace::Onset::Reader& reader) {
  kj::Vector<kj::String> results(reader.getScriptTags().size());
  for (auto tag: reader.getScriptTags()) {
    results.add(kj::str(tag));
  }
  return results.releaseAsArray();
}
kj::Maybe<kj::String> maybeGetEntrypoint(const rpc::Trace::Onset::Reader& reader) {
  if (!reader.hasEntrypoint()) return kj::none;
  return kj::str(reader.getEntrypoint());
}
kj::Maybe<EventInfo> maybeGetEventInfo(const rpc::Trace::Onset::Reader& reader) {
  auto info = reader.getInfo();
  switch (info.which()) {
    case rpc::Trace::Onset::Info::Which::NONE:
      return kj::none;
    case rpc::Trace::Onset::Info::Which::FETCH: {
      return kj::Maybe(FetchEventInfo(info.getFetch()));
    }
    case rpc::Trace::Onset::Info::Which::JS_RPC: {
      return kj::Maybe(JsRpcEventInfo(info.getJsRpc()));
    }
    case rpc::Trace::Onset::Info::Which::SCHEDULED: {
      return kj::Maybe(ScheduledEventInfo(info.getScheduled()));
    }
    case rpc::Trace::Onset::Info::Which::ALARM: {
      return kj::Maybe(AlarmEventInfo(info.getAlarm()));
    }
    case rpc::Trace::Onset::Info::Which::QUEUE: {
      return kj::Maybe(QueueEventInfo(info.getQueue()));
    }
    case rpc::Trace::Onset::Info::Which::EMAIL: {
      return kj::Maybe(EmailEventInfo(info.getEmail()));
    }
    case rpc::Trace::Onset::Info::Which::TRACE: {
      return kj::Maybe(TraceEventInfo(info.getTrace()));
    }
    case rpc::Trace::Onset::Info::Which::HIBERNATABLE_WEB_SOCKET: {
      return kj::Maybe(HibernatableWebSocketEventInfo(info.getHibernatableWebSocket()));
    }
  }
  KJ_UNREACHABLE;
}

kj::Maybe<EventInfo> cloneEventInfo(const kj::Maybe<EventInfo>& other) {
  KJ_IF_SOME(e, other) {
    KJ_SWITCH_ONEOF(e) {
      KJ_CASE_ONEOF(fetch, FetchEventInfo) {
        return kj::Maybe(fetch.clone());
      }
      KJ_CASE_ONEOF(jsRpc, JsRpcEventInfo) {
        return kj::Maybe(jsRpc.clone());
      }
      KJ_CASE_ONEOF(scheduled, ScheduledEventInfo) {
        return kj::Maybe(scheduled.clone());
      }
      KJ_CASE_ONEOF(alarm, AlarmEventInfo) {
        return kj::Maybe(alarm.clone());
      }
      KJ_CASE_ONEOF(queue, QueueEventInfo) {
        return kj::Maybe(queue.clone());
      }
      KJ_CASE_ONEOF(email, EmailEventInfo) {
        return kj::Maybe(email.clone());
      }
      KJ_CASE_ONEOF(trace, TraceEventInfo) {
        return kj::Maybe(trace.clone());
      }
      KJ_CASE_ONEOF(hibWs, HibernatableWebSocketEventInfo) {
        return kj::Maybe(hibWs.clone());
      }
      KJ_CASE_ONEOF(custom, CustomEventInfo) {
        KJ_UNREACHABLE;
      }
    }
  }
  return kj::none;
}
}  // namespace

Onset::Onset(kj::Maybe<kj::String> scriptName,
    kj::Maybe<kj::Own<ScriptVersion::Reader>> scriptVersion,
    kj::Maybe<kj::String> dispatchNamespace,
    kj::Maybe<kj::String> scriptId,
    kj::Array<kj::String> scriptTags,
    kj::Maybe<kj::String> entrypoint,
    ExecutionModel executionModel)
    : scriptName(kj::mv(scriptName)),
      scriptVersion(kj::mv(scriptVersion)),
      dispatchNamespace(kj::mv(dispatchNamespace)),
      scriptId(kj::mv(scriptId)),
      scriptTags(kj::mv(scriptTags)),
      entrypoint(kj::mv(entrypoint)),
      executionModel(executionModel) {}

Onset::Onset(rpc::Trace::Onset::Reader reader)
    : scriptName(maybeGetScriptName(reader)),
      scriptVersion(maybeGetScriptVersion(reader)),
      dispatchNamespace(maybeGetDispatchNamespace(reader)),
      scriptId(maybeGetScriptId(reader)),
      scriptTags(maybeGetScriptTags(reader)),
      entrypoint(maybeGetEntrypoint(reader)),
      executionModel(reader.getExecutionModel()),
      info(maybeGetEventInfo(reader)) {}

void Onset::copyTo(rpc::Trace::Onset::Builder builder) const {
  KJ_IF_SOME(name, scriptName) {
    builder.setScriptName(name);
  }
  KJ_IF_SOME(version, scriptVersion) {
    builder.setScriptVersion(*version);
  }
  KJ_IF_SOME(ns, dispatchNamespace) {
    builder.setDispatchNamespace(ns);
  }
  KJ_IF_SOME(id, scriptId) {
    builder.setScriptId(id);
  }
  if (scriptTags.size() > 0) {
    auto list = builder.initScriptTags(scriptTags.size());
    for (auto i: kj::indices(scriptTags)) {
      list.set(i, scriptTags[i]);
    }
  }
  KJ_IF_SOME(e, entrypoint) {
    builder.setEntrypoint(e);
  }
  builder.setExecutionModel(executionModel);

  KJ_IF_SOME(i, info) {
    auto infoBuilder = builder.initInfo();
    KJ_SWITCH_ONEOF(i) {
      KJ_CASE_ONEOF(fetch, FetchEventInfo) {
        fetch.copyTo(infoBuilder.initFetch());
      }
      KJ_CASE_ONEOF(jsRpc, JsRpcEventInfo) {
        jsRpc.copyTo(infoBuilder.initJsRpc());
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
      KJ_CASE_ONEOF(hibWs, HibernatableWebSocketEventInfo) {
        hibWs.copyTo(infoBuilder.initHibernatableWebSocket());
      }
      KJ_CASE_ONEOF(custom, CustomEventInfo) {
        KJ_UNREACHABLE;
      }
    }
  }
}

Onset Onset::clone() const {
  Onset onset(scriptName.map([](const kj::String& s) { return kj::str(s); }),
      scriptVersion.map([](const kj::Own<ScriptVersion::Reader>& s) { return capnp::clone(*s); }),
      dispatchNamespace.map([](const kj::String& s) { return kj::str(s); }),
      scriptId.map([](const kj::String& s) { return kj::str(s); }), KJ_MAP(tag, scriptTags) {
    return kj::str(tag);
  }, entrypoint.map([](const kj::String& s) { return kj::str(s); }), executionModel);
  onset.info = cloneEventInfo(info);
  return kj::mv(onset);
}

// ======================================================================================
// FetchEventInfo

namespace {
kj::HttpMethod validateMethod(capnp::HttpMethod method) {
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

FetchEventInfo FetchEventInfo::clone() const {
  kj::Vector<Header> newHeaders(headers.size());
  for (auto& header: headers) {
    newHeaders.add(Header(kj::str(header.name), kj::str(header.value)));
  }

  return FetchEventInfo(method, kj::str(url), kj::str(cfJson), newHeaders.releaseAsArray());
}

// ======================================================================================
// FetchResponseInfo
FetchResponseInfo::FetchResponseInfo(uint16_t statusCode): statusCode(statusCode) {}

FetchResponseInfo::FetchResponseInfo(rpc::Trace::FetchResponseInfo::Reader reader)
    : statusCode(reader.getStatusCode()) {}

void FetchResponseInfo::copyTo(rpc::Trace::FetchResponseInfo::Builder builder) const {
  builder.setStatusCode(statusCode);
}

FetchResponseInfo FetchResponseInfo::clone() const {
  return FetchResponseInfo(statusCode);
}

// ======================================================================================
// JsRpcEventInfo

JsRpcEventInfo::JsRpcEventInfo(kj::String methodName): methodName(kj::mv(methodName)) {}

JsRpcEventInfo::JsRpcEventInfo(rpc::Trace::JsRpcEventInfo::Reader reader)
    : methodName(kj::str(reader.getMethodName())) {}

void JsRpcEventInfo::copyTo(rpc::Trace::JsRpcEventInfo::Builder builder) const {
  builder.setMethodName(methodName);
}

JsRpcEventInfo JsRpcEventInfo::clone() const {
  return JsRpcEventInfo(kj::str(methodName));
}

// ======================================================================================
// ScheduledEventInfo

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

// ======================================================================================
// AlarmEventInfo

AlarmEventInfo::AlarmEventInfo(kj::Date scheduledTime): scheduledTime(scheduledTime) {}

AlarmEventInfo::AlarmEventInfo(rpc::Trace::AlarmEventInfo::Reader reader)
    : scheduledTime(reader.getScheduledTimeMs() * kj::MILLISECONDS + kj::UNIX_EPOCH) {}

void AlarmEventInfo::copyTo(rpc::Trace::AlarmEventInfo::Builder builder) const {
  builder.setScheduledTimeMs((scheduledTime - kj::UNIX_EPOCH) / kj::MILLISECONDS);
}

AlarmEventInfo AlarmEventInfo::clone() const {
  return AlarmEventInfo(scheduledTime);
}

// ======================================================================================
// QueueEventInfo

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

void EmailEventInfo::copyTo(rpc::Trace::EmailEventInfo::Builder builder) const {
  builder.setMailFrom(mailFrom);
  builder.setRcptTo(rcptTo);
  builder.setRawSize(rawSize);
}

EmailEventInfo EmailEventInfo::clone() const {
  return EmailEventInfo(kj::str(mailFrom), kj::str(rcptTo), rawSize);
}

// ======================================================================================
// HibernatableWebSocketEventInfo

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

HibernatableWebSocketEventInfo HibernatableWebSocketEventInfo::clone() const {
  auto newType = ([this]() -> Type {
    KJ_SWITCH_ONEOF(type) {
      KJ_CASE_ONEOF(_, Message) {
        return Message{};
      }
      KJ_CASE_ONEOF(close, Close) {
        return Close{close.code, close.wasClean};
      }
      KJ_CASE_ONEOF(_, Error) {
        return Error{};
      }
    }
    KJ_UNREACHABLE;
  })();
  return HibernatableWebSocketEventInfo(kj::mv(newType));
}

// ======================================================================================
// TraceEventInfo

namespace {
kj::Vector<TraceEventInfo::TraceItem> getTraceItemsFromTraces(kj::ArrayPtr<kj::Own<Trace>> traces) {
  kj::Vector<TraceEventInfo::TraceItem> results(traces.size());
  for (size_t n = 0; n < traces.size(); n++) {
    KJ_IF_SOME(name, traces[n]->onsetInfo.scriptName) {
      TraceEventInfo::TraceItem item(kj::str(name));
      results.add(kj::mv(item));
    } else {
      TraceEventInfo::TraceItem item(kj::str("<unknown>"_kj));
      results.add(kj::mv(item));
    }
  }
  return results.releaseAsArray();
}

kj::Vector<TraceEventInfo::TraceItem> getTraceItemsFromReader(
    rpc::Trace::TraceEventInfo::Reader reader) {
  auto traces = reader.getTraces();
  kj::Vector<TraceEventInfo::TraceItem> results;
  results.addAll(traces);
  return results.releaseAsArray();
}
}  // namespace

TraceEventInfo::TraceEventInfo(kj::ArrayPtr<kj::Own<Trace>> traces)
    : traces(getTraceItemsFromTraces(traces)) {}

TraceEventInfo::TraceEventInfo(kj::Array<TraceItem> traces): traces(kj::mv(traces)) {}

TraceEventInfo::TraceEventInfo(rpc::Trace::TraceEventInfo::Reader reader)
    : traces(getTraceItemsFromReader(reader)) {}

void TraceEventInfo::copyTo(rpc::Trace::TraceEventInfo::Builder builder) const {
  auto list = builder.initTraces(traces.size());
  for (auto i: kj::indices(traces)) {
    traces[i].copyTo(list[i]);
  }
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
  return TraceEventInfo::TraceItem(scriptName.map([](const kj::String& s) { return kj::str(s); }));
}

TraceEventInfo TraceEventInfo::clone() const {
  kj::Vector<TraceItem> newTraces(traces.size());
  for (auto& trace: traces) {
    newTraces.add(trace.clone());
  }
  return TraceEventInfo(newTraces.releaseAsArray());
}

// ======================================================================================
// DiagnosticChannelEvent

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

// ======================================================================================
// Log

Log::Log(kj::Date timestamp, LogLevel logLevel, kj::String message)
    : timestamp(timestamp),
      logLevel(logLevel),
      message(kj::mv(message)) {}

Log::Log(rpc::Trace::Log::Reader reader)
    : timestamp(kj::UNIX_EPOCH + reader.getTimestampNs() * kj::NANOSECONDS),
      logLevel(reader.getLogLevel()),
      message(kj::str(reader.getMessage())) {}

void Log::copyTo(rpc::Trace::Log::Builder builder) const {
  builder.setTimestampNs((timestamp - kj::UNIX_EPOCH) / kj::NANOSECONDS);
  builder.setLogLevel(logLevel);
  builder.setMessage(message);
}

Log Log::clone() const {
  return Log(timestamp, logLevel, kj::str(message));
}

LogV2::LogV2(LogLevel logLevel, kj::Array<kj::byte> message)
    : logLevel(logLevel),
      message(kj::mv(message)) {}

LogV2::LogV2(rpc::Trace::LogV2::Reader reader)
    : logLevel(reader.getLogLevel()),
      message(kj::heapArray<kj::byte>(reader.getMessage())) {}

void LogV2::copyTo(rpc::Trace::LogV2::Builder builder) const {
  builder.setLogLevel(logLevel);
  builder.setMessage(message);
}

LogV2 LogV2::clone() const {
  return LogV2(logLevel, kj::heapArray<kj::byte>(message));
}

// ======================================================================================
// Exception
namespace {
kj::Maybe<kj::String> maybeGetStack(const rpc::Trace::Exception::Reader& reader) {
  if (!reader.hasStack()) return kj::none;
  return kj::str(reader.getStack());
}
}  // namespace

Exception::Exception(
    kj::Date timestamp, kj::String name, kj::String message, kj::Maybe<kj::String> stack)
    : timestamp(timestamp),
      name(kj::mv(name)),
      message(kj::mv(message)),
      stack(kj::mv(stack)) {}

Exception::Exception(rpc::Trace::Exception::Reader reader)
    : timestamp(kj::UNIX_EPOCH + reader.getTimestampNs() * kj::NANOSECONDS),
      name(kj::str(reader.getName())),
      message(kj::str(reader.getMessage())),
      stack(maybeGetStack(reader)),
      remote(reader.getRemote()),
      retryable(reader.getRetryable()),
      overloaded(reader.getOverloaded()),
      durableObjectReset(reader.getDurableObjectReset()) {}

void Exception::copyTo(rpc::Trace::Exception::Builder builder) const {
  builder.setTimestampNs((timestamp - kj::UNIX_EPOCH) / kj::NANOSECONDS);
  builder.setName(name);
  builder.setMessage(message);
  KJ_IF_SOME(s, stack) {
    builder.setStack(s);
  }

  KJ_IF_SOME(c, cause) {
    c->copyTo(builder);
  }
  if (errors.size() > 0) {
    auto errorsBuilder = builder.initErrors(errors.size());
    for (size_t n = 0; n < errors.size(); n++) {
      errors[n]->copyTo(errorsBuilder[n]);
    }
  }
  builder.setRemote(remote);
  builder.setRetryable(retryable);
  builder.setOverloaded(overloaded);
  builder.setDurableObjectReset(durableObjectReset);
}

Exception Exception::clone() const {
  Exception ex(timestamp, kj::str(name), kj::str(message),
      stack.map([](const kj::String& s) { return kj::str(s); }));
  KJ_IF_SOME(c, cause) {
    ex.cause = kj::heap(c->clone());
  }
  ex.retryable = retryable;
  ex.remote = remote;
  ex.overloaded = overloaded;
  ex.durableObjectReset = durableObjectReset;
  return kj::mv(ex);
}

// ======================================================================================
// Subrequest
namespace {
kj::Maybe<Subrequest::Info> maybeGetSubrequestInfo(const rpc::Trace::Subrequest::Reader& reader) {
  auto info = reader.getInfo();
  switch (info.which()) {
    case rpc::Trace::Subrequest::Info::Which::NONE: {
      return kj::none;
    }
    case rpc::Trace::Subrequest::Info::Which::FETCH: {
      return kj::Maybe(FetchEventInfo(info.getFetch()));
    }
    case rpc::Trace::Subrequest::Info::Which::JS_RPC: {
      return kj::Maybe(JsRpcEventInfo(info.getJsRpc()));
    }
  }
  KJ_UNREACHABLE;
}
}  // namespace

Subrequest::Subrequest(kj::Maybe<Info> info): info(kj::mv(info)) {}

Subrequest::Subrequest(rpc::Trace::Subrequest::Reader reader)
    : info(maybeGetSubrequestInfo(reader)) {}

void Subrequest::copyTo(rpc::Trace::Subrequest::Builder builder) const {
  KJ_IF_SOME(i, info) {
    KJ_SWITCH_ONEOF(i) {
      KJ_CASE_ONEOF(fetch, FetchEventInfo) {
        fetch.copyTo(builder.getInfo().initFetch());
      }
      KJ_CASE_ONEOF(jsRpc, JsRpcEventInfo) {
        builder.getInfo().which();
        jsRpc.copyTo(builder.getInfo().initJsRpc());
      }
    }
  }
}

Subrequest Subrequest::clone() const {
  auto newInfo = ([&]() -> kj::Maybe<Info> {
    KJ_IF_SOME(i, info) {
      KJ_SWITCH_ONEOF(i) {
        KJ_CASE_ONEOF(fetch, FetchEventInfo) {
          return kj::Maybe(fetch.clone());
        }
        KJ_CASE_ONEOF(jsRpc, JsRpcEventInfo) {
          return kj::Maybe(jsRpc.clone());
        }
      }
      KJ_UNREACHABLE;
    }
    return kj::none;
  })();
  return Subrequest(kj::mv(newInfo));
}

// ======================================================================================
// SpanClose

namespace {
kj::Maybe<FetchResponseInfo> getSpanCloseInfo(const rpc::Trace::SpanClose::Reader& reader) {
  auto info = reader.getInfo();
  switch (info.which()) {
    case rpc::Trace::SpanClose::Info::Which::NONE: {
      return kj::none;
    }
    case rpc::Trace::SpanClose::Info::Which::FETCH: {
      return kj::Maybe(FetchResponseInfo(info.getFetch()));
    }
  }
  KJ_UNREACHABLE;
}
}  // namespace

SpanClose::SpanClose(EventOutcome outcome, kj::Maybe<FetchResponseInfo> maybeInfo)
    : outcome(outcome),
      info(kj::mv(maybeInfo)) {}

SpanClose::SpanClose(rpc::Trace::SpanClose::Reader reader)
    : outcome(reader.getOutcome()),
      info(getSpanCloseInfo(reader)) {}

void SpanClose::copyTo(rpc::Trace::SpanClose::Builder builder) const {
  builder.setOutcome(outcome);
  KJ_IF_SOME(i, info) {
    auto infoBuilder = builder.initInfo();
    i.copyTo(infoBuilder.initFetch());
  }
}

SpanClose SpanClose::clone() const {
  return SpanClose(outcome, info.map([](const FetchResponseInfo& i) { return i.clone(); }));
}

// ======================================================================================
// Metric
Metric::Metric(Type type, kj::String key, double value)
    : type(type),
      key(kj::mv(key)),
      value(kj::mv(value)) {}

Metric::Metric(rpc::Trace::Metric::Reader reader)
    : type(reader.getType()),
      key(kj::str(reader.getKey())),
      value(reader.getValue()) {}

void Metric::copyTo(rpc::Trace::Metric::Builder builder) const {
  builder.setType(type);
  builder.setKey(key);
  builder.setValue(value);
}

Metric Metric::clone() const {
  return Metric(type, kj::str(key), value);
}

}  // namespace workerd::trace
