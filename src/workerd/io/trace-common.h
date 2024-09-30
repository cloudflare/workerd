#pragma once

#include <workerd/io/outcome.capnp.h>
#include <workerd/io/worker-interface.capnp.h>
#include <workerd/jsg/memory.h>

#include <kj/compat/http.h>
#include <kj/string.h>

namespace workerd {

using LogLevel = rpc::Trace::Log::Level;
using ExecutionModel = rpc::Trace::ExecutionModel;

enum class PipelineLogLevel {
  // WARNING: This must be kept in sync with PipelineDef::LogLevel (which is not in the OSS
  //   release).
  NONE,
  FULL
};

namespace trace {

// Metadata describing the onset of a trace session.
struct OnsetInfo final {
  kj::Maybe<kj::String> ownerId = kj::none;
  kj::Maybe<kj::String> stableId = kj::none;
  kj::Maybe<kj::String> scriptName = kj::none;
  kj::Maybe<kj::Own<ScriptVersion::Reader>> scriptVersion = kj::none;
  kj::Maybe<kj::String> dispatchNamespace = kj::none;
  kj::Maybe<kj::String> scriptId = kj::none;
  kj::Array<kj::String> scriptTags = nullptr;
  kj::Maybe<kj::String> entrypoint = kj::none;
  ExecutionModel ExecutionModel;
};

// Metadata describing the start of a received fetch request.
struct FetchEventInfo final {
  struct Header;

  explicit FetchEventInfo(
      kj::HttpMethod method, kj::String url, kj::String cfJson, kj::Array<Header> headers);
  FetchEventInfo(rpc::Trace::FetchEventInfo::Reader reader);

  struct Header final {
    explicit Header(kj::String name, kj::String value);
    Header(rpc::Trace::FetchEventInfo::Header::Reader reader);

    kj::String name;
    kj::String value;

    void copyTo(rpc::Trace::FetchEventInfo::Header::Builder builder) const;

    JSG_MEMORY_INFO(Header) {
      tracker.trackField("name", name);
      tracker.trackField("value", value);
    }
  };

  kj::HttpMethod method;
  kj::String url;
  // TODO(perf): It might be more efficient to store some sort of parsed JSON result instead?
  kj::String cfJson;
  kj::Array<Header> headers;

  void copyTo(rpc::Trace::FetchEventInfo::Builder builder) const;
};

struct JsRpcEventInfo final {
  explicit JsRpcEventInfo(kj::String methodName);
  JsRpcEventInfo(rpc::Trace::JsRpcEventInfo::Reader reader);

  kj::String methodName;

  void copyTo(rpc::Trace::JsRpcEventInfo::Builder builder);
};

struct ScheduledEventInfo final {
  explicit ScheduledEventInfo(double scheduledTime, kj::String cron);
  ScheduledEventInfo(rpc::Trace::ScheduledEventInfo::Reader reader);

  double scheduledTime;
  kj::String cron;

  void copyTo(rpc::Trace::ScheduledEventInfo::Builder builder);
};

struct AlarmEventInfo final {
  explicit AlarmEventInfo(kj::Date scheduledTime);
  AlarmEventInfo(rpc::Trace::AlarmEventInfo::Reader reader);

  kj::Date scheduledTime;

  void copyTo(rpc::Trace::AlarmEventInfo::Builder builder);
};

}  // namespace trace
}  // namespace workerd
