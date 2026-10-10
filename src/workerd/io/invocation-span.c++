// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "invocation-span.h"

#include <workerd/util/thread-scopes.h>

#include <capnp/compat/json.h>
#include <capnp/message.h>
#include <kj/compat/http.h>

#include <cstdio>
#include <ctime>
#include <utility>

namespace workerd {

namespace {

// The handler type as the Onset event spells it (`fetch`, `jsrpc`, `scheduled`, ...).
kj::ConstString handlerType(const tracing::EventInfo& info) {
  KJ_SWITCH_ONEOF(info) {
    KJ_CASE_ONEOF(fetch, tracing::FetchEventInfo) {
      return "fetch"_kjc;
    }
    KJ_CASE_ONEOF(jsRpc, tracing::JsRpcEventInfo) {
      return "jsrpc"_kjc;
    }
    KJ_CASE_ONEOF(scheduled, tracing::ScheduledEventInfo) {
      return "scheduled"_kjc;
    }
    KJ_CASE_ONEOF(alarm, tracing::AlarmEventInfo) {
      return "alarm"_kjc;
    }
    KJ_CASE_ONEOF(queue, tracing::QueueEventInfo) {
      return "queue"_kjc;
    }
    KJ_CASE_ONEOF(email, tracing::EmailEventInfo) {
      return "email"_kjc;
    }
    KJ_CASE_ONEOF(trace, tracing::TraceEventInfo) {
      return "trace"_kjc;
    }
    KJ_CASE_ONEOF(hws, tracing::HibernatableWebSocketEventInfo) {
      return "hibernatableWebSocket"_kjc;
    }
    KJ_CASE_ONEOF(connect, tracing::ConnectEventInfo) {
      return "connect"_kjc;
    }
    KJ_CASE_ONEOF(custom, tracing::CustomEventInfo) {
      return "custom"_kjc;
    }
  }
  KJ_UNREACHABLE;
}

// `faas.trigger` per handler type. `connect` has no streaming tail worker counterpart and is
// reported as `http` like `fetch`.
kj::ConstString faasTrigger(const tracing::EventInfo& info) {
  KJ_SWITCH_ONEOF(info) {
    KJ_CASE_ONEOF(fetch, tracing::FetchEventInfo) {
      return "http"_kjc;
    }
    KJ_CASE_ONEOF(connect, tracing::ConnectEventInfo) {
      return "http"_kjc;
    }
    KJ_CASE_ONEOF(jsRpc, tracing::JsRpcEventInfo) {
      return "jsrpc"_kjc;
    }
    KJ_CASE_ONEOF(scheduled, tracing::ScheduledEventInfo) {
      return "timer"_kjc;
    }
    KJ_CASE_ONEOF(alarm, tracing::AlarmEventInfo) {
      return "timer"_kjc;
    }
    KJ_CASE_ONEOF(queue, tracing::QueueEventInfo) {
      return "pubsub"_kjc;
    }
    KJ_CASE_ONEOF(email, tracing::EmailEventInfo) {
      return "email"_kjc;
    }
    KJ_CASE_ONEOF(trace, tracing::TraceEventInfo) {
      return "trace"_kjc;
    }
    KJ_CASE_ONEOF(hws, tracing::HibernatableWebSocketEventInfo) {
      return "websocket"_kjc;
    }
    KJ_CASE_ONEOF(custom, tracing::CustomEventInfo) {
      return "other"_kjc;
    }
  }
  KJ_UNREACHABLE;
}

// The request's `cf` object is opaque to the runtime except for these geolocation and network
// fields, which the streaming tail worker's OTel conversion also lifts onto the root span.
void setCfTags(SpanBuilder& span, kj::StringPtr cfJson) {
  constexpr std::pair<kj::StringPtr, kj::StringPtr> STRING_FIELDS[] = {
    {"verifiedBotCategory"_kj, "cloudflare.verified_bot_category"_kj},
    {"timezone"_kj, "geo.timezone"_kj},
    {"continent"_kj, "geo.continent.code"_kj},
    {"country"_kj, "geo.country.code"_kj},
    {"city"_kj, "geo.locality.name"_kj},
    {"region"_kj, "geo.locality.region"_kj},
  };
  capnp::MallocMessageBuilder message;
  auto cf = message.initRoot<capnp::JsonValue>();
  // A `cf` blob that is not a JSON object yields no tags, like the Onset event's consumer.
  if (kj::runCatchingExceptions([&]() { capnp::JsonCodec().decodeRaw(cfJson, cf); }) != kj::none ||
      !cf.isObject()) {
    return;
  }
  for (auto field: cf.getObject()) {
    auto name = field.getName().asString();
    auto value = field.getValue();
    if (name == "asn"_kj && value.isNumber()) {
      span.setTag("cloudflare.asn"_kjc, static_cast<int64_t>(value.getNumber()));
      continue;
    }
    for (auto& [cfName, key]: STRING_FIELDS) {
      if (name == cfName && value.isString()) {
        span.setTag(kj::ConstString(kj::str(key)), kj::str(value.getString()));
      }
    }
  }
}

void setFetchTags(SpanBuilder& span, const tracing::FetchEventInfo& fetch) {
  span.setTag("http.request.method"_kjc, kj::str(fetch.method));
  span.setTag("url.full"_kjc, kj::str(fetch.url));
  // Header names are lower-cased when the event info is built (see buildFetchEventInfo()). Only
  // the headers below are recorded; the rest may carry credentials or personal data.
  for (auto& header: fetch.headers) {
    if (header.name == "user-agent"_kj) {
      span.setTag("user_agent.original"_kjc, kj::str(header.value));
    } else if (header.name == "content-length"_kj) {
      KJ_IF_SOME(size, header.value.tryParseAs<int64_t>()) {
        if (size >= 0) span.setTag("http.request.body.size"_kjc, size);
      }
    } else if (header.name == "accept"_kj || header.name == "accept-encoding"_kj ||
        header.name == "accept-language"_kj) {
      span.setTag(
          kj::ConstString(kj::str("http.request.header.", header.name)), kj::str(header.value));
    }
  }
  setCfTags(span, fetch.cfJson);
}

// `date` as the ISO 8601 string `Date.prototype.toISOString()` produces, which is how the
// streaming tail worker reports scheduled times.
kj::String isoDateString(kj::Date date) {
  int64_t millis = (date - kj::UNIX_EPOCH) / kj::MILLISECONDS;
  time_t seconds = millis / 1000;
  struct tm t;
  KJ_ASSERT(gmtime_r(&seconds, &t) == &t);
  char buffer[32]{};
  size_t size = strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &t);
  size += snprintf(buffer + size, sizeof(buffer) - size, ".%03dZ", static_cast<int>(millis % 1000));
  return kj::str(kj::arrayPtr(buffer, size));
}

// Tags specific to the handler type. Mirrors the Onset event, which reports the epoch as the
// scheduled time in predictable mode.
void setEventTags(SpanBuilder& span, const tracing::EventInfo& info) {
  bool predictable = isPredictableModeForTest();
  KJ_SWITCH_ONEOF(info) {
    KJ_CASE_ONEOF(fetch, tracing::FetchEventInfo) {
      setFetchTags(span, fetch);
    }
    KJ_CASE_ONEOF(scheduled, tracing::ScheduledEventInfo) {
      span.setTag("faas.cron"_kjc, kj::str(scheduled.cron));
      // The scheduled time is JavaScript's milliseconds since the epoch.
      span.setTag("cloudflare.scheduled_time"_kjc,
          isoDateString(predictable ? kj::UNIX_EPOCH
                                    : kj::UNIX_EPOCH +
                      static_cast<int64_t>(scheduled.scheduledTime) * kj::MILLISECONDS));
    }
    KJ_CASE_ONEOF(alarm, tracing::AlarmEventInfo) {
      span.setTag("cloudflare.scheduled_time"_kjc,
          isoDateString(predictable ? kj::UNIX_EPOCH : alarm.scheduledTime));
    }
    KJ_CASE_ONEOF(queue, tracing::QueueEventInfo) {
      span.setTag("cloudflare.queue.name"_kjc, kj::str(queue.queueName));
      span.setTag("cloudflare.queue.batch_size"_kjc, static_cast<int64_t>(queue.batchSize));
    }
    KJ_CASE_ONEOF(email, tracing::EmailEventInfo) {
      span.setTag("cloudflare.email.from"_kjc, kj::str(email.mailFrom));
      span.setTag("cloudflare.email.to"_kjc, kj::str(email.rcptTo));
      span.setTag("cloudflare.email.size"_kjc, static_cast<int64_t>(email.rawSize));
    }
    KJ_CASE_ONEOF(trace, tracing::TraceEventInfo) {
      span.setTag("cloudflare.trace.count"_kjc, static_cast<int64_t>(trace.traces.size()));
    }
    KJ_CASE_ONEOF_DEFAULT {}
  }
}

}  // namespace

void describeInvocationSpan(SpanBuilder& span, const tracing::EventInfo& info) {
  if (!span.isObserved()) return;
  KJ_IF_SOME(fetch, info.tryGet<tracing::FetchEventInfo>()) {
    span.setOperationName(kj::ConstString(kj::str(fetch.method)));
  } else {
    span.setOperationName(handlerType(info));
  }
  span.setTag("faas.trigger"_kjc, faasTrigger(info));
  span.setTag("cloudflare.handler_type"_kjc, handlerType(info));
  setEventTags(span, info);
}

void setInvocationResponseStatus(SpanBuilder& span, uint statusCode) {
  span.setTag("http.response.status_code"_kjc, static_cast<int64_t>(statusCode));
}

void setInvocationRpcMethod(SpanBuilder& span, kj::StringPtr methodName) {
  span.setTag("jsrpc.method"_kjc, kj::str(methodName));
  // Also under the key the streaming tail worker's OTel conversion uses for the same value.
  span.setTag("cloudflare.rpcMethod"_kjc, kj::str(methodName));
}

void setInvocationOutcome(
    SpanBuilder& span, EventOutcome outcome, kj::Duration cpuTime, kj::Duration wallTime) {
  span.setTag("cloudflare.outcome"_kjc, kj::str(outcome));
  // Mirror the Outcome event, which reports zero durations in predictable mode.
  bool predictable = isPredictableModeForTest();
  span.setTag(
      "cpu_time_ms"_kjc, static_cast<int64_t>(predictable ? 0 : cpuTime / kj::MILLISECONDS));
  span.setTag(
      "wall_time_ms"_kjc, static_cast<int64_t>(predictable ? 0 : wallTime / kj::MILLISECONDS));
}

}  // namespace workerd
