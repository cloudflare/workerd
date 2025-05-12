#include <workerd/api/global-scope.h>
#include <workerd/io/io-context.h>
#include <workerd/io/trace-stream.h>
#include <workerd/io/worker-interface.h>
#include <workerd/jsg/jsg.h>
#include <workerd/util/completion-membrane.h>
#include <workerd/util/uuid.h>

#include <capnp/membrane.h>

namespace workerd::tracing {
namespace {

#define STRS(V)                                                                                    \
  V(ALARM, "alarm")                                                                                \
  V(ATTACHMENT, "attachment")                                                                      \
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
  V(FETCH, "fetch")                                                                                \
  V(HEADERS, "headers")                                                                            \
  V(HIBERNATABLEWEBSOCKET, "hibernatableWebSocket")                                                \
  V(HIBERNATE, "hibernate")                                                                        \
  V(ID, "id")                                                                                      \
  V(INFO, "info")                                                                                  \
  V(INVOCATIONID, "invocationId")                                                                  \
  V(JSRPC, "jsrpc")                                                                                \
  V(KILLSWITCH, "killSwitch")                                                                      \
  V(LABEL, "label")                                                                                \
  V(LEVEL, "level")                                                                                \
  V(LINK, "link")                                                                                  \
  V(LOADSHED, "loadShed")                                                                          \
  V(LOG, "log")                                                                                    \
  V(MAILFROM, "mailFrom")                                                                          \
  V(MESSAGE, "message")                                                                            \
  V(METHOD, "method")                                                                              \
  V(METHODNAME, "methodName")                                                                      \
  V(NAME, "name")                                                                                  \
  V(OK, "ok")                                                                                      \
  V(ONSET, "onset")                                                                                \
  V(OUTCOME, "outcome")                                                                            \
  V(PARENTSPANID, "parentSpanId")                                                                  \
  V(QUEUE, "queue")                                                                                \
  V(QUEUENAME, "queueName")                                                                        \
  V(RAWSIZE, "rawSize")                                                                            \
  V(RCPTTO, "rcptTo")                                                                              \
  V(RESPONSESTREAMDISCONNECTED, "responseStreamDisconnected")                                      \
  V(RESUME, "resume")                                                                              \
  V(RETURN, "return")                                                                              \
  V(SCHEDULED, "scheduled")                                                                        \
  V(SCHEDULEDTIME, "scheduledTime")                                                                \
  V(SCRIPTNAME, "scriptName")                                                                      \
  V(SCRIPTNOTFOUND, "scriptNotFound")                                                              \
  V(SCRIPTTAGS, "scriptTags")                                                                      \
  V(SCRIPTVERSION, "scriptVersion")                                                                \
  V(SEQUENCE, "sequence")                                                                          \
  V(SPANCLOSE, "spanClose")                                                                        \
  V(SPANID, "spanId")                                                                              \
  V(SPANOPEN, "spanOpen")                                                                          \
  V(STACK, "stack")                                                                                \
  V(STATUSCODE, "statusCode")                                                                      \
  V(TAG, "tag")                                                                                    \
  V(TIMESTAMP, "timestamp")                                                                        \
  V(TRACEID, "traceId")                                                                            \
  V(TRACE, "trace")                                                                                \
  V(TRACES, "traces")                                                                              \
  V(TRIGGER, "trigger")                                                                            \
  V(TYPE, "type")                                                                                  \
  V(UNKNOWN, "unknown")                                                                            \
  V(URL, "url")                                                                                    \
  V(VALUE, "value")                                                                                \
  V(WALLTIME, "wallTime")                                                                          \
  V(WARN, "warn")                                                                                  \
  V(WASCLEAN, "wasClean")

#define V(N, L) constexpr kj::StringPtr N##_STR = L##_kj;
STRS(V)
#undef STRS

// Utility that prevents creating duplicate JS strings while serializing a tail event.
class StringCache final {
 public:
  StringCache() = default;
  KJ_DISALLOW_COPY_AND_MOVE(StringCache);

  jsg::JsValue get(jsg::Lock& js, kj::StringPtr value) {
    return cache
        .findOrCreate(value, [&]() -> decltype(cache)::Entry {
      return {value, jsg::JsRef<jsg::JsValue>(js, js.strIntern(value))};
    }).getHandle(js);
  }

 private:
  kj::HashMap<kj::StringPtr, jsg::JsRef<jsg::JsValue>> cache;
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
jsg::JsValue ToJs(jsg::Lock& js, const tracing::Attribute::Value& value) {
  KJ_SWITCH_ONEOF(value) {
    KJ_CASE_ONEOF(str, kj::String) {
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
jsg::JsValue ToJs(jsg::Lock& js, const tracing::Attribute& attribute, StringCache& cache) {
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
jsg::JsValue ToJs(
    jsg::Lock& js, kj::ArrayPtr<const tracing::Attribute> attributes, StringCache& cache) {
  auto obj = js.obj();
  obj.set(js, TYPE_STR, cache.get(js, ATTRIBUTES_STR));
  obj.set(js, INFO_STR, js.arr(attributes, [&cache](jsg::Lock& js, const auto& attr) {
    return ToJs(js, attr, cache);
  }));
  return obj;
}

jsg::JsValue ToJs(jsg::Lock& js, const tracing::FetchResponseInfo& info, StringCache& cache) {
  auto obj = js.obj();
  obj.set(js, TYPE_STR, cache.get(js, FETCH_STR));
  obj.set(js, STATUSCODE_STR, js.num(info.statusCode));
  return obj;
}

jsg::JsValue ToJs(jsg::Lock& js, const tracing::FetchEventInfo& info, StringCache& cache) {
  auto obj = js.obj();
  obj.set(js, TYPE_STR, cache.get(js, FETCH_STR));
  obj.set(js, METHOD_STR, cache.get(js, kj::str(info.method)));
  obj.set(js, URL_STR, js.str(info.url));
  obj.set(js, CFJSON_STR, js.str(info.cfJson));

  auto ToJs = [](jsg::Lock& js, const tracing::FetchEventInfo::Header& header, StringCache& cache) {
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

jsg::JsValue ToJs(jsg::Lock& js, const tracing::JsRpcEventInfo& info, StringCache& cache) {
  auto obj = js.obj();
  obj.set(js, TYPE_STR, cache.get(js, JSRPC_STR));
  obj.set(js, METHODNAME_STR, cache.get(js, info.methodName));
  return obj;
}

jsg::JsValue ToJs(jsg::Lock& js, const tracing::ScheduledEventInfo& info, StringCache& cache) {
  auto obj = js.obj();
  obj.set(js, TYPE_STR, cache.get(js, SCHEDULED_STR));
  if (isPredictableModeForTest()) {
    obj.set(js, SCHEDULEDTIME_STR, js.date(kj::UNIX_EPOCH));
  } else {
    // TODO(streaming-tail): Depending on if the schedule handler is invoked with a scheduledTime
    // argument or not, the value in the trace can be either the current time or the scheduledTime
    // parameter. Is this the correct behavior?
    obj.set(js, SCHEDULEDTIME_STR, js.date(info.scheduledTime));
  }
  obj.set(js, CRON_STR, js.str(info.cron));
  return obj;
}

jsg::JsValue ToJs(jsg::Lock& js, const tracing::AlarmEventInfo& info, StringCache& cache) {
  auto obj = js.obj();
  obj.set(js, TYPE_STR, cache.get(js, ALARM_STR));
  if (isPredictableModeForTest()) {
    obj.set(js, SCHEDULEDTIME_STR, js.date(kj::UNIX_EPOCH));
  } else {
    obj.set(js, SCHEDULEDTIME_STR, js.date(info.scheduledTime));
  }
  return obj;
}

jsg::JsValue ToJs(jsg::Lock& js, const tracing::QueueEventInfo& info, StringCache& cache) {
  auto obj = js.obj();
  obj.set(js, TYPE_STR, cache.get(js, QUEUE_STR));
  obj.set(js, QUEUENAME_STR, js.str(info.queueName));
  obj.set(js, BATCHSIZE_STR, js.num(info.batchSize));
  return obj;
}

jsg::JsValue ToJs(jsg::Lock& js, const tracing::EmailEventInfo& info, StringCache& cache) {
  auto obj = js.obj();
  obj.set(js, TYPE_STR, cache.get(js, EMAIL_STR));
  obj.set(js, MAILFROM_STR, js.str(info.mailFrom));
  obj.set(js, RCPTTO_STR, js.str(info.rcptTo));
  obj.set(js, RAWSIZE_STR, js.num(info.rawSize));
  return obj;
}

jsg::JsValue ToJs(jsg::Lock& js, const tracing::TraceEventInfo& info, StringCache& cache) {
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

jsg::JsValue ToJs(
    jsg::Lock& js, const tracing::HibernatableWebSocketEventInfo& info, StringCache& cache) {
  auto obj = js.obj();
  obj.set(js, TYPE_STR, cache.get(js, HIBERNATABLEWEBSOCKET_STR));

  KJ_SWITCH_ONEOF(info.type) {
    KJ_CASE_ONEOF(message, tracing::HibernatableWebSocketEventInfo::Message) {
      auto mobj = js.obj();
      mobj.set(js, TYPE_STR, cache.get(js, MESSAGE_STR));
      obj.set(js, INFO_STR, mobj);
    }
    KJ_CASE_ONEOF(error, tracing::HibernatableWebSocketEventInfo::Error) {
      auto mobj = js.obj();
      mobj.set(js, TYPE_STR, cache.get(js, ERROR_STR));
      obj.set(js, INFO_STR, mobj);
    }
    KJ_CASE_ONEOF(close, tracing::HibernatableWebSocketEventInfo::Close) {
      auto mobj = js.obj();
      mobj.set(js, TYPE_STR, cache.get(js, CLOSE_STR));
      mobj.set(js, CODE_STR, js.num(close.code));
      mobj.set(js, WASCLEAN_STR, js.boolean(close.wasClean));
      obj.set(js, INFO_STR, mobj);
    }
  }

  return obj;
}

jsg::JsValue ToJs(jsg::Lock& js, const tracing::Resume& info, StringCache& cache) {
  auto obj = js.obj();
  obj.set(js, TYPE_STR, cache.get(js, RESUME_STR));

  KJ_IF_SOME(attachment, info.attachment) {
    jsg::Serializer::Released data{
      .data = kj::heapArray<kj::byte>(attachment),
    };
    jsg::Deserializer deser(js, data);
    obj.set(js, ATTACHMENT_STR, deser.readValue(js));
  }

  return obj;
}

jsg::JsValue ToJs(jsg::Lock& js, const tracing::CustomEventInfo& info, StringCache& cache) {
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

jsg::JsValue ToJs(jsg::Lock& js, const tracing::Onset& onset, StringCache& cache) {
  auto obj = js.obj();
  obj.set(js, TYPE_STR, cache.get(js, ONSET_STR));

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

  KJ_IF_SOME(trigger, onset.trigger) {
    auto tobj = js.obj();
    tobj.set(js, TRACEID_STR, js.str(trigger.traceId.toGoString()));
    tobj.set(js, INVOCATIONID_STR, js.str(trigger.invocationId.toGoString()));
    tobj.set(js, SPANID_STR, js.str(trigger.spanId.toGoString()));
    obj.set(js, TRIGGER_STR, tobj);
  }

  KJ_SWITCH_ONEOF(onset.info) {
    KJ_CASE_ONEOF(fetch, tracing::FetchEventInfo) {
      obj.set(js, INFO_STR, ToJs(js, fetch, cache));
    }
    KJ_CASE_ONEOF(jsrpc, tracing::JsRpcEventInfo) {
      obj.set(js, INFO_STR, ToJs(js, jsrpc, cache));
    }
    KJ_CASE_ONEOF(scheduled, tracing::ScheduledEventInfo) {
      obj.set(js, INFO_STR, ToJs(js, scheduled, cache));
    }
    KJ_CASE_ONEOF(alarm, tracing::AlarmEventInfo) {
      obj.set(js, INFO_STR, ToJs(js, alarm, cache));
    }
    KJ_CASE_ONEOF(queue, tracing::QueueEventInfo) {
      obj.set(js, INFO_STR, ToJs(js, queue, cache));
    }
    KJ_CASE_ONEOF(email, tracing::EmailEventInfo) {
      obj.set(js, INFO_STR, ToJs(js, email, cache));
    }
    KJ_CASE_ONEOF(trace, tracing::TraceEventInfo) {
      obj.set(js, INFO_STR, ToJs(js, trace, cache));
    }
    KJ_CASE_ONEOF(hws, tracing::HibernatableWebSocketEventInfo) {
      obj.set(js, INFO_STR, ToJs(js, hws, cache));
    }
    KJ_CASE_ONEOF(resume, tracing::Resume) {
      obj.set(js, INFO_STR, ToJs(js, resume, cache));
    }
    KJ_CASE_ONEOF(custom, tracing::CustomEventInfo) {
      obj.set(js, INFO_STR, ToJs(js, custom, cache));
    }
  }

  return obj;
}

jsg::JsValue ToJs(jsg::Lock& js, const tracing::Outcome& outcome, StringCache& cache) {
  auto obj = js.obj();
  obj.set(js, TYPE_STR, cache.get(js, OUTCOME_STR));
  obj.set(js, OUTCOME_STR, ToJs(js, outcome.outcome, cache));

  double cpuTime = outcome.cpuTime / kj::MILLISECONDS;
  double wallTime = outcome.wallTime / kj::MILLISECONDS;

  obj.set(js, CPUTIME_STR, js.num(cpuTime));
  obj.set(js, WALLTIME_STR, js.num(wallTime));

  return obj;
}

jsg::JsValue ToJs(jsg::Lock& js, const tracing::Hibernate& hibernate, StringCache& cache) {
  auto obj = js.obj();
  obj.set(js, TYPE_STR, cache.get(js, HIBERNATE_STR));
  return obj;
}

jsg::JsValue ToJs(jsg::Lock& js, const tracing::SpanOpen& spanOpen, StringCache& cache) {
  auto obj = js.obj();
  obj.set(js, TYPE_STR, cache.get(js, SPANOPEN_STR));
  obj.set(js, NAME_STR, js.str(spanOpen.operationName));

  // TODO(streaming-tail): Finalize format for providing span ID/parent span ID
  if (isPredictableModeForTest()) {
    obj.set(js, PARENTSPANID_STR, js.str(kj::hex((uint64_t)0x2a2a2a2a2a2a2a2a)));
  } else {
    obj.set(js, PARENTSPANID_STR, js.str(kj::hex(spanOpen.parentSpanId)));
  }

  KJ_IF_SOME(info, spanOpen.info) {
    KJ_SWITCH_ONEOF(info) {
      KJ_CASE_ONEOF(fetch, tracing::FetchEventInfo) {
        obj.set(js, INFO_STR, ToJs(js, fetch, cache));
      }
      KJ_CASE_ONEOF(jsrpc, tracing::JsRpcEventInfo) {
        obj.set(js, INFO_STR, ToJs(js, jsrpc, cache));
      }
      KJ_CASE_ONEOF(custom, tracing::CustomInfo) {
        obj.set(js, INFO_STR, ToJs(js, custom.asPtr(), cache));
      }
    }
  }
  return obj;
}

jsg::JsValue ToJs(jsg::Lock& js, const tracing::SpanClose& spanClose, StringCache& cache) {
  auto obj = js.obj();
  obj.set(js, TYPE_STR, cache.get(js, SPANCLOSE_STR));
  obj.set(js, OUTCOME_STR, ToJs(js, spanClose.outcome, cache));
  return obj;
}

jsg::JsValue ToJs(jsg::Lock& js, const tracing::DiagnosticChannelEvent& dce, StringCache& cache) {
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

jsg::JsValue ToJs(jsg::Lock& js, const tracing::Exception& ex, StringCache& cache) {
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
    case LogLevel::ERROR:
      return cache.get(js, ERROR_STR);
    case LogLevel::INFO:
      return cache.get(js, INFO_STR);
    case LogLevel::LOG:
      return cache.get(js, LOG_STR);
    case LogLevel::WARN:
      return cache.get(js, WARN_STR);
  }
  KJ_UNREACHABLE;
}

jsg::JsValue ToJs(jsg::Lock& js, const tracing::Log& log, StringCache& cache) {
  auto obj = js.obj();
  obj.set(js, TYPE_STR, cache.get(js, LOG_STR));
  obj.set(js, LEVEL_STR, ToJs(js, log.logLevel, cache));
  obj.set(js, MESSAGE_STR, js.str(log.message));
  return obj;
}

jsg::JsValue ToJs(jsg::Lock& js, const tracing::Return& ret, StringCache& cache) {
  auto obj = js.obj();
  obj.set(js, TYPE_STR, cache.get(js, RETURN_STR));

  KJ_IF_SOME(info, ret.info) {
    KJ_SWITCH_ONEOF(info) {
      KJ_CASE_ONEOF(fetch, tracing::FetchResponseInfo) {
        obj.set(js, INFO_STR, ToJs(js, fetch, cache));
      }
      KJ_CASE_ONEOF(custom, tracing::CustomInfo) {
        obj.set(js, INFO_STR, ToJs(js, custom.asPtr(), cache));
      }
    }
  }

  return obj;
}

jsg::JsValue ToJs(jsg::Lock& js, const tracing::Link& link, StringCache& cache) {
  auto obj = js.obj();
  obj.set(js, TYPE_STR, cache.get(js, LINK_STR));
  KJ_IF_SOME(label, link.label) {
    obj.set(js, LABEL_STR, js.str(label));
  }
  obj.set(js, TRACEID_STR, js.str(link.traceId.toGoString()));
  obj.set(js, INVOCATIONID_STR, js.str(link.invocationId.toGoString()));
  obj.set(js, SPANID_STR, js.str(link.spanId.toGoString()));
  return obj;
}

jsg::JsValue ToJs(jsg::Lock& js, const tracing::TailEvent& event, StringCache& cache) {
  auto obj = js.obj();
  obj.set(js, TRACEID_STR, js.str(event.traceId.toGoString()));
  obj.set(js, INVOCATIONID_STR, js.str(event.invocationId.toGoString()));
  obj.set(js, SPANID_STR, js.str(event.spanId.toGoString()));
  obj.set(js, TIMESTAMP_STR, js.date(event.timestamp));
  obj.set(js, SEQUENCE_STR, js.num(event.sequence));

  KJ_SWITCH_ONEOF(event.event) {
    KJ_CASE_ONEOF(onset, tracing::Onset) {
      obj.set(js, EVENT_STR, ToJs(js, onset, cache));
    }
    KJ_CASE_ONEOF(outcome, tracing::Outcome) {
      obj.set(js, EVENT_STR, ToJs(js, outcome, cache));
    }
    KJ_CASE_ONEOF(hibernate, tracing::Hibernate) {
      obj.set(js, EVENT_STR, ToJs(js, hibernate, cache));
    }
    KJ_CASE_ONEOF(spanOpen, tracing::SpanOpen) {
      obj.set(js, EVENT_STR, ToJs(js, spanOpen, cache));
    }
    KJ_CASE_ONEOF(spanClose, tracing::SpanClose) {
      obj.set(js, EVENT_STR, ToJs(js, spanClose, cache));
    }
    KJ_CASE_ONEOF(de, tracing::DiagnosticChannelEvent) {
      obj.set(js, EVENT_STR, ToJs(js, de, cache));
    }
    KJ_CASE_ONEOF(ex, tracing::Exception) {
      obj.set(js, EVENT_STR, ToJs(js, ex, cache));
    }
    KJ_CASE_ONEOF(log, tracing::Log) {
      obj.set(js, EVENT_STR, ToJs(js, log, cache));
    }
    KJ_CASE_ONEOF(ret, tracing::Return) {
      obj.set(js, EVENT_STR, ToJs(js, ret, cache));
    }
    KJ_CASE_ONEOF(link, tracing::Link) {
      obj.set(js, EVENT_STR, ToJs(js, link, cache));
    }
    KJ_CASE_ONEOF(attrs, kj::Array<tracing::Attribute>) {
      obj.set(js, EVENT_STR, ToJs(js, attrs, cache));
    }
  }

  return obj;
}

// Returns the name of the handler function for this type of event.
kj::Maybe<kj::StringPtr> getHandlerName(const tracing::TailEvent& event) {
  KJ_SWITCH_ONEOF(event.event) {
    KJ_CASE_ONEOF(_, tracing::Onset) {
      KJ_FAIL_ASSERT("Onset event should only be provided to tailStream(), not returned handler");
      // return ONSET_STR;
    }
    KJ_CASE_ONEOF(_, tracing::Outcome) {
      return OUTCOME_STR;
    }
    KJ_CASE_ONEOF(_, tracing::Hibernate) {
      return HIBERNATE_STR;
    }
    KJ_CASE_ONEOF(_, tracing::SpanOpen) {
      return SPANOPEN_STR;
    }
    KJ_CASE_ONEOF(_, tracing::SpanClose) {
      return SPANCLOSE_STR;
    }
    KJ_CASE_ONEOF(_, tracing::DiagnosticChannelEvent) {
      return DIAGNOSTICCHANNEL_STR;
    }
    KJ_CASE_ONEOF(_, tracing::Exception) {
      return EXCEPTION_STR;
    }
    KJ_CASE_ONEOF(_, tracing::Log) {
      return LOG_STR;
    }
    KJ_CASE_ONEOF(_, tracing::Return) {
      return RETURN_STR;
    }
    KJ_CASE_ONEOF(_, tracing::Link) {
      return LINK_STR;
    }
    KJ_CASE_ONEOF(_, tracing::CustomInfo) {
      return ATTRIBUTES_STR;
    }
  }
  return kj::none;
}

class TailStreamTarget final: public rpc::TailStreamTarget::Server {
 public:
  TailStreamTarget(
      IoContext& ioContext, Frankenvalue props, kj::Own<kj::PromiseFulfiller<void>> doneFulfiller)
      : weakIoContext(ioContext.getWeakRef()),
        props(kj::mv(props)),
        doneFulfiller(kj::mv(doneFulfiller)) {}

  KJ_DISALLOW_COPY_AND_MOVE(TailStreamTarget);
  ~TailStreamTarget() {
    if (doneFulfiller->isWaiting()) {
      doneFulfiller->reject(KJ_EXCEPTION(DISCONNECTED, "Tail stream session canceled."));
    }
  }

  kj::Promise<void> report(ReportContext reportContext) override {
    IoContext& ioContext = KJ_REQUIRE_NONNULL(
        weakIoContext->tryGet(), "The destination object for this tail session no longer exists.");

    ioContext.getLimitEnforcer().topUpActor();

    auto ownReportContext = capnp::CallContextHook::from(reportContext).addRef();

    auto promise =
        ioContext
            .run([this, &ioContext, reportContext, ownReportContext = kj::mv(ownReportContext)](
                     Worker::Lock& lock) mutable -> kj::Promise<void> {
      auto params = reportContext.getParams();
      KJ_ASSERT(params.hasEvents(), "Events are required.");
      auto eventReaders = params.getEvents();
      kj::Vector<tracing::TailEvent> events(eventReaders.size());
      for (auto reader: eventReaders) {
        events.add(tracing::TailEvent(reader));
      }
      // TODO(streaming-tail): Timestamp handling here serves as a placeholder, proper
      // implementation will be more work, incl. addressing potential spectre concerns.
      for (auto& event: events) {
        event.timestamp = ioContext.now();
      }

      // If we have not yet received the onset event, the first event in the
      // received collection must be an Onset event and must be handled separately.
      // We will only dispatch the remaining events if a handler is returned.
      auto result = ([&]() -> kj::Promise<void> {
        KJ_IF_SOME(handler, maybeHandler) {
          auto h = handler.getHandle(lock);
          return handleEvents(
              lock, h, ioContext, events.releaseAsArray(), reportContext.initResults());
        } else {
          return handleOnset(lock, ioContext, events.releaseAsArray(), reportContext.initResults());
        }
      })();

      if (ioContext.hasOutputGate()) {
        return result.then([weakIoContext = weakIoContext->addRef()]() mutable {
          return KJ_REQUIRE_NONNULL(weakIoContext->tryGet()).waitForOutputLocks();
        });
      } else {
        return kj::mv(result);
      }
    }).catch_([](kj::Exception&& e) {
      if (jsg::isTunneledException(e.getDescription())) {
        auto description = jsg::stripRemoteExceptionPrefix(e.getDescription());
        if (!description.startsWith("remote.")) {
          e.setDescription(kj::str("remote.", description));
        }
      }
      kj::throwFatalException(kj::mv(e));
    });

    auto paf = kj::newPromiseAndFulfiller<void>();
    promise = promise.then([&fulfiller = *paf.fulfiller]() { fulfiller.fulfill(); },
        [&fulfiller = *paf.fulfiller](kj::Exception&& e) { fulfiller.reject(kj::mv(e)); });
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
  // Handles the very first (onset) event in the tail stream. This will cause
  // the exported tailStream handler to be called, passing the onset event
  // as the initial argument. If the tail stream wishes to continue receiving
  // events for this invocation, it will return a handler in the form of an
  // object or a function. If no handler is returned, the tail session is
  // shutdown.
  kj::Promise<void> handleOnset(Worker::Lock& lock,
      IoContext& ioContext,
      kj::ArrayPtr<tracing::TailEvent> events,
      rpc::TailStreamTarget::TailStreamResults::Builder results) {
    // There should be only a single onset event in this batch.
    KJ_ASSERT(events.size() == 1 && events[0].event.is<tracing::Onset>(),
        "Expected only a single onset event");
    auto& event = events[0];

    // TODO(later): This assumes the tail worker is on the default export.
    // Should we allow other named exports to provide the tailStream handler?
    auto handler = KJ_REQUIRE_NONNULL(
        lock.getExportedHandler("default"_kj, kj::mv(props), ioContext.getActor()),
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
      results.setStop(true);
      doneFulfiller->fulfill();
      return kj::READY_NOW;
    }

    // Invoke the tailStream handler function.
    v8::Local<v8::Function> fn = maybeFn.As<v8::Function>();
    auto maybeCtx = KJ_ASSERT_NONNULL(handler->getCtx()).tryGetHandle(js);
    v8::LocalVector<v8::Value> handlerArgs(js.v8Isolate, maybeCtx != kj::none ? 3 : 2);
    handlerArgs[0] = ToJs(js, event, stringCache);
    handlerArgs[1] = handler->env.getHandle(js);
    KJ_IF_SOME(ctx, maybeCtx) {
      handlerArgs[2] = ctx;
    }

    try {
      auto result =
          jsg::check(fn->Call(js.v8Context(), target, handlerArgs.size(), handlerArgs.data()));

      // We need to be able to access the results builder from both the
      // success and failure branches of the promise we set up below.
      struct SharedResults: public kj::Refcounted {
        rpc::TailStreamTarget::TailStreamResults::Builder results;
        rpc::TailStreamTarget::TailStreamResults::Builder& get() {
          return results;
        }
        SharedResults(rpc::TailStreamTarget::TailStreamResults::Builder results)
            : results(kj::mv(results)) {}
      };
      auto sharedResults = kj::rc<SharedResults>(kj::mv(results));

      // The handler can return a function, an object, undefined, or a promise
      // for any of these. We will convert the result to a promise for consistent
      // handling...
      return ioContext.awaitJs(js,
          js.toPromise(result).then(js,
              ioContext.addFunctor([this, results = sharedResults.addRef(), &ioContext](
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
          maybeHandler = jsg::JsRef(js, jsg::JsValue(handle));
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
        results->get().setStop(true);
        doneFulfiller->fulfill();
      }),
              ioContext.addFunctor([this, results = sharedResults.addRef()](
                                       jsg::Lock& js, jsg::Value&& error) mutable {
        results->get().setStop(true);
        doneFulfiller->fulfill();
        js.throwException(kj::mv(error));
      })));
    } catch (...) {
      ioContext.logWarningOnce("A worker configured to act as a streaming tail worker did "
                               "not return a valid tailStream() handler.");
      results.setStop(true);
      doneFulfiller->fulfill();
      return kj::READY_NOW;
    }
    KJ_UNREACHABLE;
  }

  kj::Promise<void> handleEvents(Worker::Lock& lock,
      const jsg::JsValue& handler,
      IoContext& ioContext,
      kj::ArrayPtr<tracing::TailEvent> events,
      rpc::TailStreamTarget::TailStreamResults::Builder results) {
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

    if (h->IsFunction()) {
      // If the handler is a function, then we'll just pass all of the events to that
      // function. If the function returns a promise and there are multiple events we
      // will not wait for each promise to resolve before calling the next iteration.
      // But we will wait for all promises to settle before returning the resolved
      // kj promise.
      auto fn = h.As<v8::Function>();
      for (auto& event: events) {
        // If we already received an outcome event, we will stop processing any
        // further events.
        if (finishing) break;
        if (event.event.is<tracing::Outcome>()) {
          finishing = true;
          results.setStop(true);
          doFulfill = true;
        };
        v8::Local<v8::Value> eventObj = ToJs(js, event, stringCache);
        returnValues.push_back(jsg::check(fn->Call(js.v8Context(), h, 1, &eventObj)));
      }
    } else {
      // If the handler is an object, then we need to know what kind of events
      // we have and look for a specific handler function for each.
      KJ_ASSERT(h->IsObject());
      jsg::JsObject obj = jsg::JsObject(h.As<v8::Object>());
      for (auto& event: events) {
        // If we already received an outcome event, we will stop processing any
        // further events.
        if (finishing) break;
        if (event.event.is<tracing::Outcome>()) {
          finishing = true;
          results.setStop(true);
          doFulfill = true;
        };
        // It is technically an error not to have a handler name here as we shouldn't
        // be reporting any events we don't know! But, there's no reason to treat it
        // as an error here.
        KJ_IF_SOME(name, getHandlerName(event)) {
          v8::Local<v8::Value> val = obj.get(js, name);
          // If the value is not a function, we'll ignore it entirely.
          if (val->IsFunction()) {
            auto fn = val.As<v8::Function>();
            v8::Local<v8::Value> eventObj = ToJs(js, event, stringCache);
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
      if (doFulfill) {
        p = p.then(js, [&](jsg::Lock& js) { doneFulfiller->fulfill(); },
            [&](jsg::Lock& js, jsg::Value&& value) {
          doneFulfiller->reject(KJ_EXCEPTION(DISCONNECTED, "Streaming tail session canceled"));
        });
      }
      return ioContext.awaitJs(js, kj::mv(p));
    }
    return kj::READY_NOW;
  }

  kj::Own<IoContext::WeakRef> weakIoContext;
  Frankenvalue props;
  // The done fulfiller is resolved when we receive the outcome event
  // or rejected if the capability is dropped before receiving the outcome
  // event.
  kj::Own<kj::PromiseFulfiller<void>> doneFulfiller;

  // The maybeHandler will be empty until we receive and process the
  // onset event.
  kj::Maybe<jsg::JsRef<jsg::JsValue>> maybeHandler;
};
}  // namespace

kj::Promise<WorkerInterface::CustomEvent::Result> TailStreamCustomEventImpl::run(
    kj::Own<IoContext::IncomingRequest> incomingRequest,
    kj::Maybe<kj::StringPtr> entrypointName,
    Frankenvalue props,
    kj::TaskSet& waitUntilTasks) {
  IoContext& ioContext = incomingRequest->getContext();
  incomingRequest->delivered();

  KJ_IF_SOME(t, incomingRequest->getWorkerTracer()) {
    t.setEventInfo(ioContext.getInvocationSpanContext(), ioContext.now(),
        TraceEventInfo(kj::Array<TraceEventInfo::TraceItem>(nullptr)));
  }

  auto [donePromise, doneFulfiller] = kj::newPromiseAndFulfiller<void>();
  capFulfiller->fulfill(
      kj::heap<TailStreamTarget>(ioContext, kj::mv(props), kj::mv(doneFulfiller)));

  // What is happening here? I'm glad you asked! When this method is called we are
  // starting a tail stream session. Our TailStreamTarget created above is an RPC
  // server that accepts events over time as individual RPC calls. Those always
  // start with an onset event and should always end with an outcome event. It is
  // possible for the client stub to be dropped early before the outcome event
  // is delivered.
  //
  // When either the outcome event is received, or if the client stub is dropped
  // early, the donePromise should be fulfilled or rejected. Below we arrange for
  // the IoContext for the tail stream request to remain alive until the donePromise
  // is settled. We also block completion of the call to run on the same condition.
  //
  // Attaching the registerPendingEvent() to the promise is necessary to keep the
  // IoContext alive during the times our tail worker is idle waiting for more
  // events to be delivered.
  kj::ForkedPromise<void> forked = donePromise.fork();
  ioContext.addWaitUntil(forked.addBranch().attach(ioContext.registerPendingEvent()));

  KJ_DEFER({
    // waitUntil() should allow extending execution on the server side even when the client
    // disconnects.
    waitUntilTasks.add(incomingRequest->drain().attach(kj::mv(incomingRequest)));
  });

  co_await forked.addBranch().exclusiveJoin(ioContext.onAbort());
  co_return WorkerInterface::CustomEvent::Result{.outcome = EventOutcome::OK};
}

kj::Promise<WorkerInterface::CustomEvent::Result> TailStreamCustomEventImpl::sendRpc(
    capnp::HttpOverCapnpFactory& httpOverCapnpFactory,
    capnp::ByteStreamFactory& byteStreamFactory,
    rpc::EventDispatcher::Client dispatcher) {
  auto revokePaf = kj::newPromiseAndFulfiller<void>();

  // TODO(streaming-tail): Session is currently being reported as canceled when using
  // TailStreamCustomEventImpl via RPC, investigate.
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

  this->capFulfiller->fulfill(kj::mv(cap));

  try {
    co_await sent.ignoreResult().exclusiveJoin(kj::mv(completionPaf.promise));
  } catch (...) {
    auto e = kj::getCaughtExceptionAsKj();
    if (revokePaf.fulfiller->isWaiting()) {
      revokePaf.fulfiller->reject(kj::cp(e));
    }
    kj::throwFatalException(kj::mv(e));
  }

  co_return WorkerInterface::CustomEvent::Result{.outcome = EventOutcome::OK};
}

void TailStreamWriterState::reportImpl(tracing::TailEvent&& event) {
  // In reportImpl, our inner state must be active.
  auto& actives = KJ_ASSERT_NONNULL(inner.tryGet<kj::Array<kj::Own<Active>>>());

  // We only care about sessions that are currently active.
  kj::Vector<kj::Own<Active>> alive(actives.size());
  for (auto& active: actives) {
    if (active->capability != kj::none) {
      alive.add(kj::mv(active));
    }
  }

  if (alive.size() == 0) {
    // Oh! We have no active sessions. Well, never mind then, let's
    // transition to a closed state and drop everything on the floor.
    inner = Closed{};

    // Since we have no more living sessions (e.g. because all tail workers failed to return a valid
    // handler), mark the state as closing as we can't handle future events anyway.
    closing = true;
    return;
  }

  // If we're already closing, no further events should be reported.
  if (closing) return;
  if (event.event.is<tracing::Outcome>()) {
    closing = true;
  }

  // Deliver the event to the queue and make sure we are processing.
  for (auto& active: alive) {
    active->queue.push_back(event.clone());
    if (!active->pumping) {
      waitUntilTasks.add(pump(kj::addRef(*active)));
    }
  }

  inner = alive.releaseAsArray();
}

// Delivers the queued tail events to a streaming tail worker.
kj::Promise<void> TailStreamWriterState::pump(kj::Own<Active> current) {
  current->pumping = true;
  KJ_DEFER(current->pumping = false);

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
    KJ_ASSERT(!current->queue.empty());
    auto onsetEvent = kj::mv(current->queue.front());
    current->queue.pop_front();
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
    for (auto& event: current->queue) {
      event.copyTo(eventsBuilder[n++]);
    }
    current->queue.clear();

    auto result = co_await builder.send();

    // Note that although we cleared the current.queue above, it is
    // possible/likely that additional events were added to the queue
    // while the above builder.sent() was being awaited. If the result
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
}

// If we are using streaming tail workers, initialize the mechanism that will deliver events
// to that collection of tail workers.
kj::Maybe<kj::Own<tracing::TailStreamWriter>> initializeTailStreamWriter(
    kj::Array<kj::Own<WorkerInterface>> streamingTailWorkers, kj::TaskSet& waitUntilTasks) {
  if (streamingTailWorkers.size() == 0) {
    return kj::none;
  }

  auto state = kj::heap<TailStreamWriterState>(kj::mv(streamingTailWorkers), waitUntilTasks);

  return kj::refcounted<tracing::TailStreamWriter>(
      // This lambda is called for every streaming tail event that is reported. We use
      // the TailStreamWriterState for this stream to actually handle the event.
      // Pay attention to the ownership of state here. The lambda holds a bare
      // reference while the instance is attached to the kj::Own below.
      [&state = *state, &waitUntilTasks](tracing::TailEvent&& event) mutable {
    KJ_SWITCH_ONEOF(state.inner) {
      KJ_CASE_ONEOF(closed, TailStreamWriterState::Closed) {
        // The tail stream has already been closed because we have received either
        // an outcome or hibernate event. The writer should have failed and we
        // actually shouldn't get here. Assert!
        KJ_FAIL_ASSERT("tracing::TailStreamWriter report callback invoked after close");
      }
      KJ_CASE_ONEOF(pending, TailStreamWriterState::Pending) {
        // This is our first event! It has to be an onset event, which the writer
        // should have validated for us. Assert if it is not an onset then proceed
        // to start each of our tail working sessions.
        KJ_ASSERT(event.event.is<tracing::Onset>(), "First event must be an onset.");

        // Transitions into the active state by grabbing the pending client capability.
        state.inner = KJ_MAP(wi, pending) {
          auto customEvent = kj::heap<tracing::TailStreamCustomEventImpl>();
          auto result = customEvent->getCap();
          waitUntilTasks.add(
              wi->customEvent(kj::mv(customEvent)).attach(kj::mv(wi)).ignoreResult());
          return kj::refcounted<TailStreamWriterState::Active>(kj::mv(result));
        };

        // At this point our writer state is "active", which means the state
        // consists of one or more streaming tail worker client stubs to which
        // the event will be dispatched.
      }
      KJ_CASE_ONEOF(active, kj::Array<kj::Own<TailStreamWriterState::Active>>) {
        // Event cannot be a onset, which should have been validated by the writer.
        KJ_ASSERT(!event.event.is<tracing::Onset>(), "Only the first event can be an onset");
      }
    }
    state.reportImpl(kj::mv(event));

    // The state is determined to be closing when it receives a terminal event
    // like tracing::Outcome or tracing::Hibernate. If we return true, then the
    // writer expects more events to be received. If we return false, then the
    // writer can release any state it is holding because we don't expect any
    // more events to be dispatched. The writer should handle that case by
    // dropping this lambda.

    return !state.closing;
  }, []() -> kj::Date {
    // TODO(streaming-tail): Return proper timestamps. This callback is used to
    // acquire the timestamps used in the tail stream events. Ideally this will
    // use the same timesource that backs `IoContext::now()` and includes the
    // same spectre mitigations.
    return kj::UNIX_EPOCH;
  }).attach(kj::mv(state));
}

}  // namespace workerd::tracing
