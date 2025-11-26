#include <workerd/api/global-scope.h>
#include <workerd/io/io-context.h>
#include <workerd/io/io-own.h>
#include <workerd/io/trace-stream.h>
#include <workerd/io/worker-interface.h>
#include <workerd/jsg/jsg.h>
#include <workerd/util/completion-membrane.h>
#include <workerd/util/strings.h>
#include <workerd/util/uuid.h>

#include <capnp/membrane.h>

#include <algorithm>

namespace workerd::tracing {
namespace {

#define STRS(V)                                                                                    \
  V(ALARM, "alarm")                                                                                \
  V(ATTRIBUTES, "attributes")                                                                      \
  V(BATCHSIZE, "batchSize")                                                                        \
  V(CANCELED, "canceled")                                                                          \
  V(CHANNEL, "channel")                                                                            \
  V(CFJSON, "cfJson")                                                                              \
  V(CLOSE, "close")                                                                                \
  V(CODE, "code")                                                                                  \
  V(CPUTIME, "cpuTime")                                                                            \
  V(CRON, "cron")                                                                                  \
  V(CUSTOM, "custom")                                                                              \
  V(DAEMONDOWN, "daemonDown")                                                                      \
  V(DEBUG, "debug")                                                                                \
  V(DIAGNOSTICCHANNEL, "diagnosticChannel")                                                        \
  V(DISPATCHNAMESPACE, "dispatchNamespace")                                                        \
  V(EMAIL, "email")                                                                                \
  V(ENTRYPOINT, "entrypoint")                                                                      \
  V(ERROR, "error")                                                                                \
  V(EVENT, "event")                                                                                \
  V(EXCEEDEDCPU, "exceededCpu")                                                                    \
  V(EXCEEDEDMEMORY, "exceededMemory")                                                              \
  V(EXCEPTION, "exception")                                                                        \
  V(EXECUTIONMODEL, "executionModel")                                                              \
  V(FETCH, "fetch")                                                                                \
  V(HEADERS, "headers")                                                                            \
  V(HIBERNATABLEWEBSOCKET, "hibernatableWebSocket")                                                \
  V(ID, "id")                                                                                      \
  V(INFO, "info")                                                                                  \
  V(INVOCATIONID, "invocationId")                                                                  \
  V(JSRPC, "jsrpc")                                                                                \
  V(KILLSWITCH, "killSwitch")                                                                      \
  V(LEVEL, "level")                                                                                \
  V(LOADSHED, "loadShed")                                                                          \
  V(LOG, "log")                                                                                    \
  V(MAILFROM, "mailFrom")                                                                          \
  V(MESSAGE, "message")                                                                            \
  V(METHOD, "method")                                                                              \
  V(NAME, "name")                                                                                  \
  V(OK, "ok")                                                                                      \
  V(ONSET, "onset")                                                                                \
  V(OUTCOME, "outcome")                                                                            \
  V(QUEUE, "queue")                                                                                \
  V(QUEUENAME, "queueName")                                                                        \
  V(RAWSIZE, "rawSize")                                                                            \
  V(RCPTTO, "rcptTo")                                                                              \
  V(RESPONSESTREAMDISCONNECTED, "responseStreamDisconnected")                                      \
  V(RETURN, "return")                                                                              \
  V(SCHEDULED, "scheduled")                                                                        \
  V(SCHEDULEDTIME, "scheduledTime")                                                                \
  V(SCRIPTNAME, "scriptName")                                                                      \
  V(SCRIPTNOTFOUND, "scriptNotFound")                                                              \
  V(SCRIPTTAGS, "scriptTags")                                                                      \
  V(SCRIPTVERSION, "scriptVersion")                                                                \
  V(SEQUENCE, "sequence")                                                                          \
  V(SPANCLOSE, "spanClose")                                                                        \
  V(SPANCONTEXT, "spanContext")                                                                    \
  V(SPANID, "spanId")                                                                              \
  V(SPANOPEN, "spanOpen")                                                                          \
  V(STACK, "stack")                                                                                \
  V(STATUSCODE, "statusCode")                                                                      \
  V(TAG, "tag")                                                                                    \
  V(TIMESTAMP, "timestamp")                                                                        \
  V(TRACEID, "traceId")                                                                            \
  V(TRACE, "trace")                                                                                \
  V(TRACES, "traces")                                                                              \
  V(TYPE, "type")                                                                                  \
  V(UNKNOWN, "unknown")                                                                            \
  V(URL, "url")                                                                                    \
  V(VALUE, "value")                                                                                \
  V(WALLTIME, "wallTime")                                                                          \
  V(WARN, "warn")                                                                                  \
  V(WASCLEAN, "wasClean")

#define V(N, L) constexpr kj::LiteralStringConst N##_STR = L##_kjc;
STRS(V)
#undef STRS

// Utility that prevents creating duplicate JS strings while serializing a tail event.
class StringCache final {
 public:
  StringCache() = default;
  KJ_DISALLOW_COPY_AND_MOVE(StringCache);

  // Inserted string keys must live as long as the cache. For string constants (the common case),
  // we use LiteralStringConst and avoid memory allocation. For temporary strings, we pass in a
  // StringPtr and allocate a string. Having ConstString as the value type fits both cases.
  jsg::JsValue get(jsg::Lock& js, kj::LiteralStringConst value) {
    return cache
        .findOrCreate(value, [&]() -> decltype(cache)::Entry {
      return {value, jsg::JsRef<jsg::JsValue>(js, js.strIntern(value))};
    }).getHandle(js);
  }
  jsg::JsValue get(jsg::Lock& js, kj::StringPtr value) {
    return cache
        .findOrCreate(value, [&]() -> decltype(cache)::Entry {
      return {kj::ConstString(kj::str(value)), jsg::JsRef<jsg::JsValue>(js, js.strIntern(value))};
    }).getHandle(js);
  }

 private:
  kj::HashMap<kj::ConstString, jsg::JsRef<jsg::JsValue>> cache;
};

// Why ToJS(...) functions and not JSG_STRUCT? Good question. The various tracing:*
// types are defined in the "trace" bazel target which currently does not depend on
// jsg in any way. These also represent the internal API of these types which doesn't
// really match exactly what we want to expose to users. In order to use JSG_STRUCT
// we would either need to make the "trace" target depend on "jsg", which seems a bit
// wasteful and unnecessary, or we'd need to define wrapper structs that use JSG_STRUCT
// which also seems wasteful and unnecessary. We also don't need the type mapping for
// these structs to be bidirectional. So, instead, let's just do the simple easy thing
// and define a set of serializers to these types.

// Serialize attribute value
jsg::JsValue ToJs(jsg::Lock& js, const Attribute::Value& value) {
  KJ_SWITCH_ONEOF(value) {
    KJ_CASE_ONEOF(str, kj::ConstString) {
      return js.str(str);
    }
    KJ_CASE_ONEOF(b, bool) {
      return js.boolean(b);
    }
    KJ_CASE_ONEOF(d, double) {
      return js.num(d);
    }
    KJ_CASE_ONEOF(i, int64_t) {
      return js.bigInt(i);
    }
  }
  KJ_UNREACHABLE;
}

// Serialize attribute key:value(s) pair object
jsg::JsValue ToJs(jsg::Lock& js, const Attribute& attribute, StringCache& cache) {
  auto obj = js.obj();
  obj.set(js, NAME_STR, cache.get(js, attribute.name));

  if (attribute.value.size() == 1) {
    obj.set(js, VALUE_STR, ToJs(js, attribute.value[0]));
  } else {
    obj.set(js, VALUE_STR, js.arr(attribute.value.asPtr(), [](jsg::Lock& js, const auto& val) {
      return ToJs(js, val);
    }));
  }

  return obj;
}

// Serialize "attributes" event
jsg::JsValue ToJs(jsg::Lock& js, kj::ArrayPtr<const Attribute> attributes, StringCache& cache) {
  auto obj = js.obj();
  obj.set(js, TYPE_STR, cache.get(js, ATTRIBUTES_STR));
  obj.set(js, INFO_STR, js.arr(attributes, [&cache](jsg::Lock& js, const auto& attr) {
    return ToJs(js, attr, cache);
  }));
  return obj;
}

jsg::JsValue ToJs(jsg::Lock& js, const FetchResponseInfo& info, StringCache& cache) {
  static const kj::StringPtr keys[] = {TYPE_STR, STATUSCODE_STR};
  jsg::JsValue values[] = {cache.get(js, FETCH_STR), js.num(info.statusCode)};
  return js.obj(kj::arrayPtr(keys), kj::arrayPtr(values));
}

jsg::JsValue ToJs(jsg::Lock& js, const FetchEventInfo& info, StringCache& cache) {
  auto obj = js.obj();
  obj.set(js, TYPE_STR, cache.get(js, FETCH_STR));
  obj.set(js, METHOD_STR, cache.get(js, kj::str(info.method)));
  obj.set(js, URL_STR, js.str(info.url));
  if (info.cfJson.size() > 0) {
    obj.set(js, CFJSON_STR, jsg::JsValue(js.parseJson(info.cfJson).getHandle(js)));
  }

  auto ToJs = [](jsg::Lock& js, const FetchEventInfo::Header& header, StringCache& cache) {
    auto obj = js.obj();
    obj.set(js, NAME_STR, cache.get(js, header.name));
    obj.set(js, VALUE_STR, js.str(header.value));
    return obj;
  };

  obj.set(js, HEADERS_STR,
      js.arr(info.headers.asPtr(),
          [&cache, &ToJs](jsg::Lock& js, const auto& header) { return ToJs(js, header, cache); }));

  return obj;
}

jsg::JsValue ToJs(jsg::Lock& js, const JsRpcEventInfo& info, StringCache& cache) {
  static const kj::StringPtr keys[] = {TYPE_STR};
  jsg::JsValue values[] = {cache.get(js, JSRPC_STR)};
  return js.obj(kj::arrayPtr(keys), kj::arrayPtr(values));
}

jsg::JsValue ToJs(jsg::Lock& js, const ScheduledEventInfo& info, StringCache& cache) {
  auto obj = js.obj();
  obj.set(js, TYPE_STR, cache.get(js, SCHEDULED_STR));
  if (isPredictableModeForTest()) {
    obj.set(js, SCHEDULEDTIME_STR, js.date(kj::UNIX_EPOCH));
  } else {
    obj.set(js, SCHEDULEDTIME_STR, js.date(info.scheduledTime));
  }
  obj.set(js, CRON_STR, js.str(info.cron));
  return obj;
}

jsg::JsValue ToJs(jsg::Lock& js, const AlarmEventInfo& info, StringCache& cache) {
  auto obj = js.obj();
  obj.set(js, TYPE_STR, cache.get(js, ALARM_STR));
  if (isPredictableModeForTest()) {
    obj.set(js, SCHEDULEDTIME_STR, js.date(kj::UNIX_EPOCH));
  } else {
    obj.set(js, SCHEDULEDTIME_STR, js.date(info.scheduledTime));
  }
  return obj;
}

jsg::JsValue ToJs(jsg::Lock& js, const QueueEventInfo& info, StringCache& cache) {
  static const kj::StringPtr keys[] = {TYPE_STR, QUEUENAME_STR, BATCHSIZE_STR};
  jsg::JsValue values[] = {
    cache.get(js, QUEUE_STR), js.str(info.queueName), js.num(info.batchSize)};
  return js.obj(kj::arrayPtr(keys), kj::arrayPtr(values));
}

jsg::JsValue ToJs(jsg::Lock& js, const EmailEventInfo& info, StringCache& cache) {
  static const kj::StringPtr keys[] = {TYPE_STR, MAILFROM_STR, RCPTTO_STR, RAWSIZE_STR};
  jsg::JsValue values[] = {
    cache.get(js, EMAIL_STR), js.str(info.mailFrom), js.str(info.rcptTo), js.num(info.rawSize)};
  return js.obj(kj::arrayPtr(keys), kj::arrayPtr(values));
}

jsg::JsValue ToJs(jsg::Lock& js, const TraceEventInfo& info, StringCache& cache) {
  auto obj = js.obj();
  obj.set(js, TYPE_STR, cache.get(js, TRACE_STR));
  obj.set(js, TRACES_STR,
      js.arr(info.traces.asPtr(), [](jsg::Lock& js, const auto& trace) -> jsg::JsValue {
    KJ_IF_SOME(name, trace.scriptName) {
      return js.str(name);
    }
    return js.null();
  }));
  return obj;
}

jsg::JsValue ToJs(jsg::Lock& js, const HibernatableWebSocketEventInfo& info, StringCache& cache) {
  auto obj = js.obj();
  obj.set(js, TYPE_STR, cache.get(js, HIBERNATABLEWEBSOCKET_STR));

  KJ_SWITCH_ONEOF(info.type) {
    KJ_CASE_ONEOF(message, HibernatableWebSocketEventInfo::Message) {
      auto mobj = js.obj();
      mobj.set(js, TYPE_STR, cache.get(js, MESSAGE_STR));
      obj.set(js, INFO_STR, mobj);
    }
    KJ_CASE_ONEOF(error, HibernatableWebSocketEventInfo::Error) {
      auto mobj = js.obj();
      mobj.set(js, TYPE_STR, cache.get(js, ERROR_STR));
      obj.set(js, INFO_STR, mobj);
    }
    KJ_CASE_ONEOF(close, HibernatableWebSocketEventInfo::Close) {
      auto mobj = js.obj();
      mobj.set(js, TYPE_STR, cache.get(js, CLOSE_STR));
      mobj.set(js, CODE_STR, js.num(close.code));
      mobj.set(js, WASCLEAN_STR, js.boolean(close.wasClean));
      obj.set(js, INFO_STR, mobj);
    }
  }

  return obj;
}

jsg::JsValue ToJs(jsg::Lock& js, const CustomEventInfo& info, StringCache& cache) {
  auto obj = js.obj();
  obj.set(js, TYPE_STR, cache.get(js, CUSTOM_STR));
  return obj;
}

jsg::JsValue ToJs(jsg::Lock& js, const EventOutcome& outcome, StringCache& cache) {
  switch (outcome) {
    case EventOutcome::OK:
      return cache.get(js, OK_STR);
    case EventOutcome::CANCELED:
      return cache.get(js, CANCELED_STR);
    case EventOutcome::EXCEPTION:
      return cache.get(js, EXCEPTION_STR);
    case EventOutcome::KILL_SWITCH:
      return cache.get(js, KILLSWITCH_STR);
    case EventOutcome::DAEMON_DOWN:
      return cache.get(js, DAEMONDOWN_STR);
    case EventOutcome::EXCEEDED_CPU:
      return cache.get(js, EXCEEDEDCPU_STR);
    case EventOutcome::EXCEEDED_MEMORY:
      return cache.get(js, EXCEEDEDMEMORY_STR);
    case EventOutcome::LOAD_SHED:
      return cache.get(js, LOADSHED_STR);
    case EventOutcome::RESPONSE_STREAM_DISCONNECTED:
      return cache.get(js, RESPONSESTREAMDISCONNECTED_STR);
    case EventOutcome::SCRIPT_NOT_FOUND:
      return cache.get(js, SCRIPTNOTFOUND_STR);
    case EventOutcome::UNKNOWN:
      return cache.get(js, UNKNOWN_STR);
  }
  KJ_UNREACHABLE;
}

jsg::JsValue ToJs(jsg::Lock& js, const Onset& onset, StringCache& cache) {
  auto obj = js.obj();
  obj.set(js, TYPE_STR, cache.get(js, ONSET_STR));
  obj.set(js, EXECUTIONMODEL_STR, cache.get(js, kj::str(onset.workerInfo.executionModel)));
  obj.set(js, SPANID_STR, js.str(onset.spanId.toGoString()));

  KJ_IF_SOME(ns, onset.workerInfo.dispatchNamespace) {
    obj.set(js, DISPATCHNAMESPACE_STR, js.str(ns));
  }
  KJ_IF_SOME(entrypoint, onset.workerInfo.entrypoint) {
    obj.set(js, ENTRYPOINT_STR, js.str(entrypoint));
  }
  KJ_IF_SOME(name, onset.workerInfo.scriptName) {
    obj.set(js, SCRIPTNAME_STR, js.str(name));
  }
  KJ_IF_SOME(tags, onset.workerInfo.scriptTags) {
    obj.set(js, SCRIPTTAGS_STR,
        js.arr(tags.asPtr(), [](jsg::Lock& js, const kj::String& tag) { return js.str(tag); }));
  }
  KJ_IF_SOME(version, onset.workerInfo.scriptVersion) {
    auto vobj = js.obj();
    auto id = version->getId();
    KJ_IF_SOME(uuid, UUID::fromUpperLower(id.getUpper(), id.getLower())) {
      vobj.set(js, ID_STR, js.str(uuid.toString()));
    }
    if (version->hasTag()) {
      vobj.set(js, TAG_STR, js.str(version->getTag()));
    }
    if (version->hasMessage()) {
      vobj.set(js, MESSAGE_STR, js.str(version->getMessage()));
    }
    obj.set(js, SCRIPTVERSION_STR, vobj);
  }

  KJ_SWITCH_ONEOF(onset.info) {
    KJ_CASE_ONEOF(fetch, FetchEventInfo) {
      obj.set(js, INFO_STR, ToJs(js, fetch, cache));
    }
    KJ_CASE_ONEOF(jsrpc, JsRpcEventInfo) {
      obj.set(js, INFO_STR, ToJs(js, jsrpc, cache));
    }
    KJ_CASE_ONEOF(scheduled, ScheduledEventInfo) {
      obj.set(js, INFO_STR, ToJs(js, scheduled, cache));
    }
    KJ_CASE_ONEOF(alarm, AlarmEventInfo) {
      obj.set(js, INFO_STR, ToJs(js, alarm, cache));
    }
    KJ_CASE_ONEOF(queue, QueueEventInfo) {
      obj.set(js, INFO_STR, ToJs(js, queue, cache));
    }
    KJ_CASE_ONEOF(email, EmailEventInfo) {
      obj.set(js, INFO_STR, ToJs(js, email, cache));
    }
    KJ_CASE_ONEOF(trace, TraceEventInfo) {
      obj.set(js, INFO_STR, ToJs(js, trace, cache));
    }
    KJ_CASE_ONEOF(hws, HibernatableWebSocketEventInfo) {
      obj.set(js, INFO_STR, ToJs(js, hws, cache));
    }
    KJ_CASE_ONEOF(custom, CustomEventInfo) {
      obj.set(js, INFO_STR, ToJs(js, custom, cache));
    }
  }

  if (onset.attributes.size() > 0) {
    obj.set(js, ATTRIBUTES_STR,
        js.arr(onset.attributes.asPtr(),
            [&cache](jsg::Lock& js, const auto& attr) { return ToJs(js, attr, cache); }));
  }

  return obj;
}

jsg::JsValue ToJs(jsg::Lock& js, const Outcome& outcome, StringCache& cache) {
  auto obj = js.obj();
  obj.set(js, TYPE_STR, cache.get(js, OUTCOME_STR));
  obj.set(js, OUTCOME_STR, ToJs(js, outcome.outcome, cache));

  double cpuTime = outcome.cpuTime / kj::MILLISECONDS;
  double wallTime = outcome.wallTime / kj::MILLISECONDS;

  obj.set(js, CPUTIME_STR, js.num(cpuTime));
  obj.set(js, WALLTIME_STR, js.num(wallTime));

  return obj;
}

jsg::JsValue ToJs(jsg::Lock& js, const SpanOpen& spanOpen, StringCache& cache) {
  auto obj = js.obj();
  obj.set(js, TYPE_STR, cache.get(js, SPANOPEN_STR));
  obj.set(js, NAME_STR, js.str(spanOpen.operationName));
  // Export span ID as non-truncated hex value – in practice this will be a random span ID.
  obj.set(js, SPANID_STR, js.str(spanOpen.spanId.toGoString()));

  KJ_IF_SOME(info, spanOpen.info) {
    KJ_SWITCH_ONEOF(info) {
      KJ_CASE_ONEOF(fetch, FetchEventInfo) {
        obj.set(js, INFO_STR, ToJs(js, fetch, cache));
      }
      KJ_CASE_ONEOF(jsrpc, JsRpcEventInfo) {
        obj.set(js, INFO_STR, ToJs(js, jsrpc, cache));
      }
      KJ_CASE_ONEOF(custom, CustomInfo) {
        obj.set(js, INFO_STR, ToJs(js, custom.asPtr(), cache));
      }
    }
  }
  return obj;
}

jsg::JsValue ToJs(jsg::Lock& js, const SpanClose& spanClose, StringCache& cache) {
  auto obj = js.obj();
  obj.set(js, TYPE_STR, cache.get(js, SPANCLOSE_STR));
  obj.set(js, OUTCOME_STR, ToJs(js, spanClose.outcome, cache));
  return obj;
}

jsg::JsValue ToJs(jsg::Lock& js, const DiagnosticChannelEvent& dce, StringCache& cache) {
  auto obj = js.obj();
  obj.set(js, TYPE_STR, cache.get(js, DIAGNOSTICCHANNEL_STR));
  obj.set(js, CHANNEL_STR, cache.get(js, dce.channel));
  jsg::Serializer::Released released{
    .data = kj::heapArray<kj::byte>(dce.message),
  };
  jsg::Deserializer deser(js, released);
  obj.set(js, MESSAGE_STR, deser.readValue(js));
  return obj;
}

jsg::JsValue ToJs(jsg::Lock& js, const Exception& ex, StringCache& cache) {
  auto obj = js.obj();
  obj.set(js, TYPE_STR, cache.get(js, EXCEPTION_STR));
  obj.set(js, NAME_STR, cache.get(js, ex.name));
  obj.set(js, MESSAGE_STR, js.str(ex.message));
  KJ_IF_SOME(stack, ex.stack) {
    obj.set(js, STACK_STR, js.str(stack));
  }
  return obj;
}

jsg::JsValue ToJs(jsg::Lock& js, const LogLevel& level, StringCache& cache) {
  switch (level) {
    case LogLevel::DEBUG_:
      return cache.get(js, DEBUG_STR);
    case LogLevel::INFO:
      return cache.get(js, INFO_STR);
    case LogLevel::LOG:
      return cache.get(js, LOG_STR);
    case LogLevel::WARN:
      return cache.get(js, WARN_STR);
    case LogLevel::ERROR:
      return cache.get(js, ERROR_STR);
  }
  KJ_UNREACHABLE;
}

jsg::JsValue ToJs(jsg::Lock& js, const Log& log, StringCache& cache) {
  auto obj = js.obj();
  obj.set(js, TYPE_STR, cache.get(js, LOG_STR));
  obj.set(js, LEVEL_STR, ToJs(js, log.logLevel, cache));
  // TODO(o11y): Check that we are always returning an object here
  obj.set(js, MESSAGE_STR, jsg::JsValue(js.parseJson(log.message).getHandle(js)));
  return obj;
}

jsg::JsValue ToJs(jsg::Lock& js, const Return& ret, StringCache& cache) {
  auto obj = js.obj();
  obj.set(js, TYPE_STR, cache.get(js, RETURN_STR));

  KJ_IF_SOME(info, ret.info) {
    obj.set(js, INFO_STR, ToJs(js, info, cache));
  }

  return obj;
}

jsg::JsValue ToJs(jsg::Lock& js, const TailEvent& event, StringCache& cache) {
  auto obj = js.obj();

  // Set SpanContext
  auto sCObj = js.obj();
  sCObj.set(js, TRACEID_STR, js.str(event.spanContext.getTraceId().toGoString()));
  KJ_IF_SOME(spanId, event.spanContext.getSpanId()) {
    sCObj.set(js, SPANID_STR, js.str(spanId.toGoString()));
  }
  obj.set(js, SPANCONTEXT_STR, kj::mv(sCObj));

  obj.set(js, INVOCATIONID_STR, js.str(event.invocationId.toGoString()));
  obj.set(js, TIMESTAMP_STR, js.date(event.timestamp));
  obj.set(js, SEQUENCE_STR, js.num(event.sequence));

  KJ_SWITCH_ONEOF(event.event) {
    KJ_CASE_ONEOF(onset, Onset) {
      obj.set(js, EVENT_STR, ToJs(js, onset, cache));
    }
    KJ_CASE_ONEOF(outcome, Outcome) {
      obj.set(js, EVENT_STR, ToJs(js, outcome, cache));
    }
    KJ_CASE_ONEOF(spanOpen, SpanOpen) {
      obj.set(js, EVENT_STR, ToJs(js, spanOpen, cache));
    }
    KJ_CASE_ONEOF(spanClose, SpanClose) {
      obj.set(js, EVENT_STR, ToJs(js, spanClose, cache));
    }
    KJ_CASE_ONEOF(de, DiagnosticChannelEvent) {
      obj.set(js, EVENT_STR, ToJs(js, de, cache));
    }
    KJ_CASE_ONEOF(ex, Exception) {
      obj.set(js, EVENT_STR, ToJs(js, ex, cache));
    }
    KJ_CASE_ONEOF(log, Log) {
      obj.set(js, EVENT_STR, ToJs(js, log, cache));
    }
    KJ_CASE_ONEOF(ret, Return) {
      obj.set(js, EVENT_STR, ToJs(js, ret, cache));
    }
    KJ_CASE_ONEOF(attrs, CustomInfo) {
      obj.set(js, EVENT_STR, ToJs(js, attrs, cache));
    }
  }

  return obj;
}

// Returns the name of the handler function for this type of event.
kj::Maybe<kj::StringPtr> getHandlerName(const TailEvent& event) {
  KJ_SWITCH_ONEOF(event.event) {
    KJ_CASE_ONEOF(_, Onset) {
      KJ_FAIL_ASSERT("Onset event should only be provided to tailStream(), not returned handler");
      // return ONSET_STR;
    }
    KJ_CASE_ONEOF(_, Outcome) {
      return OUTCOME_STR;
    }
    KJ_CASE_ONEOF(_, SpanOpen) {
      return SPANOPEN_STR;
    }
    KJ_CASE_ONEOF(_, SpanClose) {
      return SPANCLOSE_STR;
    }
    KJ_CASE_ONEOF(_, DiagnosticChannelEvent) {
      return DIAGNOSTICCHANNEL_STR;
    }
    KJ_CASE_ONEOF(_, Exception) {
      return EXCEPTION_STR;
    }
    KJ_CASE_ONEOF(_, Log) {
      return LOG_STR;
    }
    KJ_CASE_ONEOF(_, Return) {
      return RETURN_STR;
    }
    KJ_CASE_ONEOF(_, CustomInfo) {
      return ATTRIBUTES_STR;
    }
  }
  return kj::none;
}

class TailStreamTarget final: public rpc::TailStreamTarget::Server {
 public:
  TailStreamTarget(IoContext& ioContext,
      kj::Maybe<kj::StringPtr> entrypointNamePtr,
      Frankenvalue props,
      kj::Own<kj::PromiseFulfiller<void>> doneFulfiller)
      : weakIoContext(ioContext.getWeakRef()),
        entrypointNamePtr(kj::mv(entrypointNamePtr)),
        props(kj::mv(props)),
        doneFulfiller(kj::mv(doneFulfiller)) {}

  KJ_DISALLOW_COPY_AND_MOVE(TailStreamTarget);
  ~TailStreamTarget() {
    if (doneFulfiller->isWaiting()) {
      doneFulfiller->reject(KJ_EXCEPTION(DISCONNECTED, "Streaming tail session canceled."));
    }
  }

  kj::Promise<void> report(ReportContext reportContext) override {
    IoContext& ioContext = KJ_REQUIRE_NONNULL(weakIoContext->tryGet(),
        "The destination object for this tail session no longer exists.", doneReceiving);

    ioContext.getLimitEnforcer().topUpActor();

    auto ownReportContext = capnp::CallContextHook::from(reportContext).addRef();
    // We need to be able to access the results builder from both the promise below and its
    // exception handler.
    auto sharedResults = kj::rc<SharedResults>(reportContext.initResults());

    auto promise = ioContext.run([this, &ioContext, sharedResults = sharedResults.addRef(),
                                     reportContext, ownReportContext = ownReportContext->addRef()](
                                     Worker::Lock& lock) mutable -> kj::Promise<void> {
      auto params = reportContext.getParams();
      KJ_ASSERT(params.hasEvents(), "Events are required.");
      auto eventReaders = params.getEvents();
      kj::Array<TailEvent> events = KJ_MAP(reader, eventReaders) { return TailEvent(reader); };

      // If we have not yet received the onset event, the first event in the
      // received collection must be an Onset event and must be handled separately.
      // We will only dispatch the remaining events if a handler is returned.
      auto result = ([&]() -> kj::Promise<void> {
        KJ_IF_SOME(handler, maybeHandler) {
          KJ_IF_SOME(h, handler.tryGet()) {
            auto handle = h.getHandle(lock);
            return handleEvents(lock, handle, ioContext, kj::mv(events), kj::mv(sharedResults));
          } else {
            KJ_LOG(ERROR, "tail stream handler was destroyed while processing events");
            JSG_FAIL_REQUIRE(Error, "Tail stream handler became invalid during event processing");
            KJ_UNREACHABLE;
          }
        } else {
          return handleOnset(lock, ioContext, kj::mv(events), kj::mv(sharedResults));
        }
      })();

      if (ioContext.hasOutputGate()) {
        return result.then([weakIoContext = weakIoContext->addRef()]() mutable {
          return KJ_REQUIRE_NONNULL(weakIoContext->tryGet()).waitForOutputLocks();
        });
      } else {
        return kj::mv(result);
      }
    });

    auto paf = kj::newPromiseAndFulfiller<void>();
    promise = promise.then([&fulfiller = *paf.fulfiller]() { fulfiller.fulfill(); },
        [&, &fulfiller = *paf.fulfiller, ownReportContext = kj::mv(ownReportContext),
            results = kj::mv(sharedResults)](kj::Exception&& e) mutable {
      // This is the top level exception catcher for tail events being delivered. We do not want to
      // propagate JS exceptions to the client side here, all exceptions should stay within this
      // customEvent. Instead, we propagate the exception to the doneFulfiller, where it is used to
      // set the right outcome code and re-thrown if appropriate. By rejecting the doneFulfiller, we
      // also ensure that no more tail events get delivered.
      if (jsg::isTunneledException(e.getDescription())) {
        auto description = jsg::stripRemoteExceptionPrefix(e.getDescription());
        if (!description.startsWith("remote.")) {
          e.setDescription(kj::str("remote.", description));
        }
      }
      // We still fulfill this fulfiller to disarm the cancellation check below
      fulfiller.fulfill();
      results->setStop(true);
      doneReceiving = true;
      doneFulfiller->reject(kj::mv(e));
    });
    promise = promise.attach(kj::defer([fulfiller = kj::mv(paf.fulfiller)]() mutable {
      if (fulfiller->isWaiting()) {
        fulfiller->reject(JSG_KJ_EXCEPTION(FAILED, Error,
            "The destination execution context for this tail session was canceled while the "
            "call was still running."));
      }
    }));
    ioContext.addTask(kj::mv(promise));

    return kj::mv(paf.promise);
  }

 private:
  // Used to share the results builder (and send the stop signal) from both the main code path and
  // the exception handler.
  struct SharedResults: public kj::Refcounted, rpc::TailStreamTarget::TailStreamResults::Builder {
    SharedResults(rpc::TailStreamTarget::TailStreamResults::Builder results)
        : rpc::TailStreamTarget::TailStreamResults::Builder(kj::mv(results)) {}
  };
  // Handles the very first (onset) event in the tail stream. This will cause
  // the exported tailStream handler to be called, passing the onset event
  // as the initial argument. If the tail stream wishes to continue receiving
  // events for this invocation, it will return a handler in the form of an
  // object or a function. If no handler is returned, the tail session is
  // shutdown.
  kj::Promise<void> handleOnset(Worker::Lock& lock,
      IoContext& ioContext,
      kj::Array<TailEvent> events,
      kj::Rc<SharedResults> results) {
    // There should be only a single onset event in this batch.
    KJ_ASSERT(
        events.size() == 1 && events[0].event.is<Onset>(), "Expected only a single onset event");
    auto& event = events[0];

    auto handler = KJ_REQUIRE_NONNULL(
        lock.getExportedHandler(entrypointNamePtr, kj::mv(props), ioContext.getActor()),
        "Failed to get handler to worker.");
    StringCache stringCache;

    jsg::Lock& js = lock;
    auto target = jsg::JsObject(handler->self.getHandle(js));
    v8::Local<v8::Value> maybeFn = target.get(js, "tailStream"_kj);

    // If there's no actual tailStream handler, or if the tailStream export is
    // something other than a function, we will emit a warning for the user
    // then immediately return.
    if (!maybeFn->IsFunction()) {
      ioContext.logWarningOnce("A worker configured to act as a streaming tail worker does "
                               "not export a tailStream() handler.");
      results->setStop(true);
      doneReceiving = true;
      doneFulfiller->fulfill();
      return kj::READY_NOW;
    }

    // Invoke the tailStream handler function.
    v8::Local<v8::Function> fn = maybeFn.As<v8::Function>();
    kj::Maybe<v8::Local<v8::Object>> maybeCtx;
    KJ_IF_SOME(hCtx, handler->getCtx()) {
      maybeCtx = v8::Local<v8::Object>(
          lock.getWorker().getIsolate().getApi().wrapExecutionContext(js, kj::mv(hCtx)));
    }
    v8::LocalVector<v8::Value> handlerArgs(js.v8Isolate, maybeCtx != kj::none ? 3 : 2);
    handlerArgs[0] = ToJs(js, event, stringCache);
    handlerArgs[1] = handler->env.getHandle(js);
    KJ_IF_SOME(ctx, maybeCtx) {
      handlerArgs[2] = ctx;
    }

    try {
      auto result =
          jsg::check(fn->Call(js.v8Context(), target, handlerArgs.size(), handlerArgs.data()));

      // The handler can return a function, an object, undefined, or a promise
      // for any of these. We will convert the result to a promise for consistent
      // handling...
      return ioContext.awaitJs(js,
          js.toPromise(result).then(js,
              ioContext.addFunctor([this, results = results.addRef(), &ioContext](
                                       jsg::Lock& js, jsg::Value value) mutable {
        // The value here can be one of a function, an object, or undefined.
        // Any value other than these will result in a warning but will otherwise
        // be treated like undefined.

        // If a function or object is returned, then our tail worker wishes to
        // keep receiving events! Yay! Otherwise, we will stop the stream by
        // setting the stop field in the results.
        auto handle = value.getHandle(js);
        if (handle->IsFunction() || handle->IsObject()) {
          // Sweet! Our tail worker wants to keep receiving events. Let's store
          // the handler and return.
          maybeHandler = ioContext.addObjectReverse(
              kj::heap<jsg::JsRef<jsg::JsValue>>(js, jsg::JsValue(handle)));
          return;
        }

        // If the handler returned any other kind of value, let's be nice and
        // at least warn the user about it.
        if (!handle->IsUndefined()) {
          ioContext.logWarningOnce(
              kj::str("tailStream() handler returned an unusable value. "
                      "The tailStream() handler is expected to return either a function, an "
                      "object, or undefined. Received ",
                  jsg::JsValue(handle).typeOf(js)));
        }
        // And finally, we'll stop the stream since the tail worker did not return
        // a handler for us to continue with.
        results->setStop(true);
        doneReceiving = true;
        doneFulfiller->fulfill();
      }),
              ioContext.addFunctor(
                  [&, results = results.addRef()](jsg::Lock& js, jsg::Value&& error) mutable {
        // Received a JS error. Do not reject doneFulfiller yet, this will be handled when we catch
        // the exception later.
        results->setStop(true);
        doneReceiving = true;
        js.throwException(kj::mv(error));
      })));
    } catch (...) {
      ioContext.logWarningOnce("A worker configured to act as a streaming tail worker did "
                               "not return a valid tailStream() handler.");
      results->setStop(true);
      doneReceiving = true;
      doneFulfiller->fulfill();
      return kj::READY_NOW;
    }
    KJ_UNREACHABLE;
  }

  kj::Promise<void> handleEvents(Worker::Lock& lock,
      const jsg::JsValue& handler,
      IoContext& ioContext,
      kj::Array<TailEvent> events,
      kj::Rc<SharedResults> results) {
    jsg::Lock& js = lock;

    // Should not ever happen but let's handle it anyway.
    if (events.size() == 0) return kj::READY_NOW;

    // Take the received set of events and dispatch them to the correct handler.

    v8::Local<v8::Value> h = handler;
    v8::LocalVector<v8::Value> returnValues(js.v8Isolate);
    StringCache stringCache;

    // If any of the events delivered are an outcome event, we will signal that
    // the stream should be stopped and will fulfill the done promise.
    bool finishing = false;

    // When a tail worker receives its outcome event, we need to ensure that the final tail worker
    // invocation is completed before destroying the tail worker customEvent and incomingRequest. To
    // achieve this, we only fulfill the doneFulfiller after JS execution has completed.
    bool doFulfill = false;

    for (auto& event: events) {
      // If we already received an outcome event, we will stop processing any
      // further events.
      if (finishing) break;
      if (event.event.is<Outcome>()) {
        finishing = true;
        results->setStop(true);
        doneReceiving = true;
        // We set doFulfill to indicate that the outcome event has been received via RPC and no more
        // events are expected.
        doFulfill = true;
      };

      v8::Local<v8::Value> eventObj = ToJs(js, event, stringCache);
      if (h->IsFunction()) {
        // If the handler is a function, then we'll just pass all of the events to that
        // function. If the function returns a promise and there are multiple events we
        // will not wait for each promise to resolve before calling the next iteration.
        // But we will wait for all promises to settle before returning the resolved
        // kj promise.
        auto fn = h.As<v8::Function>();
        returnValues.push_back(jsg::check(fn->Call(js.v8Context(), h, 1, &eventObj)));
      } else {
        // If the handler is an object, then we need to know what kind of events
        // we have and look for a specific handler function for each.
        KJ_ASSERT(h->IsObject());
        KJ_IF_SOME(name, getHandlerName(event)) {
          jsg::JsObject obj = jsg::JsObject(h.As<v8::Object>());
          v8::Local<v8::Value> val = obj.get(js, name);
          // If the value is not a function, we'll ignore it entirely.
          if (val->IsFunction()) {
            auto fn = val.As<v8::Function>();
            returnValues.push_back(jsg::check(fn->Call(js.v8Context(), h, 1, &eventObj)));
          }
        }
      }
    }
    // We want the equivalent behavior to Promise.all([...]) here but v8 does not
    // give us a C++ equivalent of Promise.all([...]) so we need to approximate it.
    // We do so by chaining all of the promises together.
    kj::Maybe<jsg::Promise<void>> promise;
    for (auto& val: returnValues) {
      KJ_IF_SOME(p, promise) {
        promise = p.then(js,
            [p = js.toPromise(val).whenResolved(js)](jsg::Lock& js) mutable { return kj::mv(p); });
      } else {
        promise = js.toPromise(val).whenResolved(js);
      }
    }

    KJ_IF_SOME(p, promise) {
      // When doFulfill is set, the last promise refers to the outcome event. In that case the chain
      // of promises provides all remaining events to the user tail handler, so we should fulfill
      // the doneFulfiller afterwards, indicating that TailStreamTarget has received all events over
      // the stream and has done all its work, that the stream self-evidently did not get canceled
      // prematurely. This applies even if promises were rejected.
      // No need to catch exceptions here: They will be handled in report() alongside exceptions
      // from the onset event etc. JSG knows how JS exceptions look like, so we don't need an
      // identifier for them.
      if (doFulfill) {
        p = p.then(js, [&](jsg::Lock& js) {
          doneReceiving = true;
          doneFulfiller->fulfill();
        });
      }
      return ioContext.awaitJs(js, kj::mv(p));
    }
    return kj::READY_NOW;
  }

  kj::Own<IoContext::WeakRef> weakIoContext;
  kj::Maybe<kj::StringPtr> entrypointNamePtr;
  Frankenvalue props;
  // The done fulfiller is resolved when we receive the outcome event
  // or rejected if the capability is dropped before receiving the outcome
  // event.
  kj::Own<kj::PromiseFulfiller<void>> doneFulfiller;

  // The maybeHandler will be empty until we receive and process the
  // onset event.
  kj::Maybe<ReverseIoOwn<jsg::JsRef<jsg::JsValue>>> maybeHandler;

  // Indicates that we told (or should have told) the client that we want no further events, used
  // to debug events arriving when the IoContext is no longer valid.
  bool doneReceiving = false;
};
}  // namespace

EventInfo TailStreamCustomEvent::getEventInfo() const {
  return TraceEventInfo(kj::Array<TraceEventInfo::TraceItem>(nullptr));
}

kj::Promise<WorkerInterface::CustomEvent::Result> TailStreamCustomEvent::run(
    kj::Own<IoContext::IncomingRequest> incomingRequest,
    kj::Maybe<kj::StringPtr> entrypointName,
    Frankenvalue props,
    kj::TaskSet& waitUntilTasks) {
  IoContext& ioContext = incomingRequest->getContext();
  incomingRequest->delivered();

  auto [donePromise, doneFulfiller] = kj::newPromiseAndFulfiller<void>();
  capFulfiller->fulfill(kj::heap<TailStreamTarget>(
      ioContext, kj::mv(entrypointName), kj::mv(props), kj::mv(doneFulfiller)));

  donePromise = donePromise.attach(ioContext.registerPendingEvent());

  KJ_DEFER({
    // waitUntil() should allow extending execution on the server side even when the client
    // disconnects.
    waitUntilTasks.add(incomingRequest->drain().attach(kj::mv(incomingRequest)));
  });

  auto eventOutcome = co_await donePromise.exclusiveJoin(ioContext.onAbort()).then([&]() {
    return ioContext.waitUntilStatus();
  }, [&incomingRequest](kj::Exception&& e) {
    // If we have a JSG exception, just set the appropriate return code – this will already have
    // been logged and we do not need to treat it like a KJ exception. Otherwise, re-throw the
    // exception.
    if (jsg::isTunneledException(e.getDescription())) {
      incomingRequest->getMetrics().reportFailure(e);
      return EventOutcome::EXCEPTION;
    }
    kj::throwRecoverableException(kj::mv(e));
    KJ_UNREACHABLE;
  });
  KJ_IF_SOME(t, ioContext.getWorkerTracer()) {
    t.setReturn(ioContext.now());
  }

  co_return WorkerInterface::CustomEvent::Result{.outcome = eventOutcome};
}

kj::Promise<WorkerInterface::CustomEvent::Result> TailStreamCustomEvent::sendRpc(
    capnp::HttpOverCapnpFactory& httpOverCapnpFactory,
    capnp::ByteStreamFactory& byteStreamFactory,
    rpc::EventDispatcher::Client dispatcher) {
  auto revokePaf = kj::newPromiseAndFulfiller<void>();

  KJ_DEFER({
    if (revokePaf.fulfiller->isWaiting()) {
      revokePaf.fulfiller->reject(KJ_EXCEPTION(DISCONNECTED, "Streaming tail session canceled"));
    }
  });

  auto req = dispatcher.tailStreamSessionRequest();
  auto sent = req.send();

  rpc::TailStreamTarget::Client cap = sent.getTopLevel();

  cap = capnp::membrane(kj::mv(cap), kj::refcounted<RevokerMembrane>(kj::mv(revokePaf.promise)));

  auto completionPaf = kj::newPromiseAndFulfiller<void>();
  cap = capnp::membrane(
      kj::mv(cap), kj::refcounted<CompletionMembrane>(kj::mv(completionPaf.fulfiller)));

  capFulfiller->fulfill(kj::mv(cap));

  // Forked promise for completion of all capabilities associated with the cap stream. This is
  // expected to be resolved when the request is canceled or when the client receives the stop
  // signal and deallocates cap after the tail worker indicates that it has processed all events
  // successfully.
  kj::ForkedPromise<void> forked = completionPaf.promise.fork();
  try {
    EventOutcome outcome = co_await sent.then([](auto resp) {
      return resp.getResult();
    }).exclusiveJoin(forked.addBranch().then([]() { return EventOutcome::CANCELED; }));

    // If the sent promise returned first, we still need to wait for the parent process to drop the
    // capability (which should happen right after it receives the stop signal) so that no
    // capabilities remain in an incomplete state when we return.
    co_await forked.addBranch();
    co_return WorkerInterface::CustomEvent::Result{.outcome = outcome};
  } catch (...) {
    auto e = kj::getCaughtExceptionAsKj();
    if (revokePaf.fulfiller->isWaiting()) {
      revokePaf.fulfiller->reject(kj::cp(e));
    }
    kj::throwFatalException(kj::mv(e));
  }
}

TailStreamWriter::TailStreamWriter(Pending pending, kj::TaskSet& waitUntilTasks)
    : inner(kj::mv(pending)),
      waitUntilTasks(waitUntilTasks) {}

bool TailStreamWriter::reportImpl(TailEvent&& event, size_t sizeHint) {
  // In reportImpl, our inner state must be active.
  auto& actives = KJ_ASSERT_NONNULL(inner.tryGet<kj::Vector<kj::Own<Active>>>());

  // We only care about sessions that are currently active, removing any inactive ones.
  auto activeEnd = std::remove_if(actives.begin(), actives.end(),
      [](const auto& active) { return active->capability == kj::none; });
  if (activeEnd == actives.begin()) {
    // Oh! We have no active sessions. Well, never mind then, let's
    // transition to a closed state and drop everything on the floor.
    inner = Closed{};

    // Since we have no more living sessions (e.g. because all tail workers failed to return a valid
    // handler), mark the state as closing as we can't handle future events anyway.
    return true;
  }

  // We have at least some active sessions. Truncate the array to get rid of any inactive ones.
  if (activeEnd != actives.end()) {
    actives.truncate(activeEnd - actives.begin());
  }

  // We do not expect any events after the outcome.
  bool isClosing = event.event.is<Outcome>();
  // Deliver the event to the queue and make sure we are processing.
  for (auto& active: actives) {
    // Only queue the event if we don't have an excessive queue size yet. Return and Outcome
    // events are only provided once and thus won't be dropped.
    if (active->queueSize < maxQueueSize || event.event.is<Outcome>() || event.event.is<Return>()) {
      // When we get to the outcome, no more events will be dropped. Inject an internal structured
      // log indicating how many events were dropped if applicable.
      if (event.event.is<Outcome>() && active->droppedEvents > 0) {
        auto log = kj::str(
            "{\"$\":\"cloudflare-streaming-tail-workers-internal\",\"type\":\"dropped\",\"count\":",
            active->droppedEvents, "}");
        TailEvent droppedEventsLog(event.spanContext.clone(), event.invocationId, event.timestamp,
            event.sequence, tracing::Log(event.timestamp, LogLevel::WARN, kj::mv(log)));
        active->queue.push(kj::mv(droppedEventsLog));
        // Increment the outcome sequence number to keep things consistent.
        event.sequence++;
      }

      // Optimization: Elide copy for last tail worker, helpful for common case of only one STW
      // being present.
      if (&active == &actives.back()) {
        active->queue.push(kj::mv(event));
      } else {
        active->queue.push(event.clone());
      }
      // Adjust estimated queue size based on size hint and an arbitrary amount for serialization
      // overhead. As long as this estimate is reasonably accurate, we won't need to check the
      // size again when serializing the message.
      active->queueSize += tailSerializationOverhead + sizeHint;
    } else {
      active->droppedEvents++;
    }

    if (!active->pumping) {
      waitUntilTasks.add(pump(kj::addRef(*active)));
    }
  }

  return isClosing;
}

// Delivers the queued tail events to a streaming tail worker.
//
// Note: An invocation of pump() may outlive the TailStreamWriter, as it is placed in
//   `waitUntilTasks`. Hence, it is declared `static`, and owns a strong ref to its `Active`.
kj::Promise<void> TailStreamWriter::pump(kj::Own<Active> current) {
  current->pumping = true;
  KJ_DEFER(current->pumping = false);

  try {
    if (!current->onsetSeen) {
      // Our first event... yay! Our first job here will be to dispatch
      // the onset event to the tail worker. If the tail worker wishes
      // to handle the remaining events in the stream, then it will return
      // a new capability to which those would be reported. This is done
      // via the "result.getPipeline()" API below. If hasPipeline()
      // returns false then that means the tail worker did not return
      // a handler for this stream and no further attempts to deliver
      // events should be made for this stream.
      current->onsetSeen = true;
      auto onsetEvent = KJ_ASSERT_NONNULL(current->queue.pop());
      auto builder = KJ_ASSERT_NONNULL(current->capability).reportRequest();
      auto eventsBuilder = builder.initEvents(1);
      // When sending the onset event to the tail worker, the receiving end
      // requires that the onset event be delivered separately, without any
      // other events in the bundle. So here we'll separate it out and deliver
      // just the one event...
      onsetEvent.copyTo(eventsBuilder[0]);
      auto result = co_await builder.send();
      if (result.getStop()) {
        // If our call to send returns a stop signal, then we'll clear
        // the capability and be done.
        current->queue.clear();
        current->capability = kj::none;
        co_return;
      }
    }

    // If we got this far then we have a handler for all of our events.
    // Deliver remaining streaming tail events in batches if possible.
    while (!current->queue.empty()) {
      auto builder = KJ_ASSERT_NONNULL(current->capability).reportRequest();
      auto eventsBuilder = builder.initEvents(current->queue.size());
      size_t n = 0;

      // We're synchronously draining the queue – reset its size.
      current->queueSize = 0;
      current->queue.drainTo([&](TailEvent&& event) { event.copyTo(eventsBuilder[n++]); });

      auto result = co_await builder.send();

      // Note that although we cleared the current.queue above, it is
      // possible/likely that additional events were added to the queue
      // while the above builder.send() was being awaited. If the result
      // comes back indicating that we should stop, then we'll stop here
      // without any further processing. We'll defensively clear the
      // queue again and drop the client stub. Otherwise, if result.getStop()
      // is false, we'll loop back around to send any items that have since
      // been added to the queue or exit this loop if there are no additional
      // events waiting to be sent.
      if (result.getStop()) {
        current->queue.clear();
        current->capability = kj::none;
        co_return;
      }
    }
  } catch (...) {
    // If any RPC throws an exception, we should treat it as a stop signal, as this suggests
    // the connection to the STW itself has been lost. (An excpetion thrown within the STW
    // itself would have resulted in a `stop` return value instead of an exception over RPC.)
    current->queue.clear();
    current->capability = kj::none;
    throw;
  }
}

// If we are using streaming tail workers, initialize the mechanism that will deliver events
// to that collection of tail workers.
kj::Maybe<kj::Own<TailStreamWriter>> initializeTailStreamWriter(
    kj::Array<kj::Own<WorkerInterface>> streamingTailWorkers, kj::TaskSet& waitUntilTasks) {
  if (streamingTailWorkers.size() == 0) {
    return kj::none;
  }

  return kj::heap<TailStreamWriter>(kj::mv(streamingTailWorkers), waitUntilTasks);
}

void TailStreamWriter::report(const InvocationSpanContext& context,
    TailEvent::Event&& event,
    kj::Date timestamp,
    size_t sizeHint) {
  // Becomes a no-op if a terminal event (close) has been reported, or if the stream closed due to
  // not receiving a well-formed event handler. We need to disambiguate these cases as the former
  // indicates an implementation error resulting in trailing events whereas the latter case is
  // caused by a user error and events being reported after the stream being closed are expected –
  // reject events following an outcome event, but otherwise just exit if the state has been closed.
  // This could be an assert, but just log an error in case this is prevalent in some edge case.
  if (outcomeSeen) {
    KJ_LOG(ERROR, "reported tail stream event after stream close ", event, kj::getStackTrace());
  }
  if (inner.is<Closed>()) {
    return;
  }
  // The onset event must be first and must only happen once.
  if (event.is<Onset>()) {
    KJ_ASSERT(!onsetSeen, "Tail stream onset already provided");
    onsetSeen = true;
  } else {
    KJ_ASSERT(onsetSeen, "Tail stream onset was not reported");
    if (event.is<Outcome>()) {
      outcomeSeen = true;
    }
  }

  // A zero spanId at the TailEvent level signifies that no spanId should be provided to the tail
  // worker (for Onset events). We go to great lengths to rule out getting an all-zero spanId by
  // chance (see SpanId::fromEntropy()), so this should be safe.
  TailEvent tailEvent(context.getTraceId(), context.getInvocationId(),
      context.getSpanId() == SpanId::nullId ? kj::none : kj::Maybe(context.getSpanId()), timestamp,
      sequence++, kj::mv(event));

  KJ_SWITCH_ONEOF(inner) {
    KJ_CASE_ONEOF(closed, Closed) {
      // The tail stream has already been closed because we have received an outcome event. The
      // writer should have failed and we actually shouldn't get here. Assert!
      KJ_FAIL_ASSERT("tracing::TailStreamWriter report callback invoked after close");
    }
    KJ_CASE_ONEOF(pending, Pending) {
      // This is our first event! It has to be an onset event as we have validated above. Start each
      // of our tail working sessions.

      // Transitions into the active state by grabbing the pending client capability.
      inner = kj::Vector<kj::Own<Active>>( KJ_MAP(wi, pending) {
        auto customEvent = kj::heap<TailStreamCustomEvent>();
        auto result = customEvent->getCap();
        auto active = kj::refcounted<Active>(kj::mv(result));

        // Attach the workerInterface and customEvent to the waitUntil tasks so that they stay alive
        // until tail worker operations including JS execution are complete, including returning the
        // outcome.
        waitUntilTasks.add(wi->customEvent(kj::mv(customEvent))
                               .attach(kj::mv(wi), kj::addRef(*active))
                               .ignoreResult());
        return active;
      });

      // At this point our writer state is "active", which means the state consists of one or more
      // streaming tail worker client stubs to which the event will be dispatched.
    }
    KJ_CASE_ONEOF(active, kj::Vector<kj::Own<Active>>) {
      // active tail stream writers have already been configured, process the event.
    }
  }

  // The state is determined to be closing when it receives a terminal event (tracing::Outcome),
  // or if there are no active tail workers left, we can close the internal state at that point.
  if (reportImpl(kj::mv(tailEvent), sizeHint)) {
    inner = Closed{};
  }
}

}  // namespace workerd::tracing
