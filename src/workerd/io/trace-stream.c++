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
  V(ATTRIBUTE, "attribute")                                                                        \
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
  V(OP, "op")                                                                                      \
  V(OUTCOME, "outcome")                                                                            \
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
    KJ_CASE_ONEOF(i, int32_t) {
      return js.num(i);
    }
  }
  KJ_UNREACHABLE;
}

jsg::JsValue ToJs(jsg::Lock& js, const tracing::Attribute& attribute, StringCache& cache) {
  auto obj = js.obj();
  obj.set(js, TYPE_STR, cache.get(js, ATTRIBUTE_STR));
  obj.set(js, NAME_STR, cache.get(js, attribute.name));

  if (attribute.value.size() == 1) {
    obj.set(js, VALUE_STR, ToJs(js, attribute.value[0]));
  } else {
    auto values = KJ_MAP(val, attribute.value) { return ToJs(js, val); };
    obj.set(js, VALUE_STR, js.arr(values));
  }

  return obj;
}

jsg::JsValue ToJs(
    jsg::Lock& js, kj::ArrayPtr<const tracing::Attribute> attributes, StringCache& cache) {
  auto attrs = KJ_MAP(attr, attributes) { return ToJs(js, attr, cache); };
  return js.arr(attrs);
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

  auto headers = KJ_MAP(header, info.headers) -> jsg::JsValue { return ToJs(js, header, cache); };
  obj.set(js, HEADERS_STR, js.arr(headers));
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
  obj.set(js, SCHEDULEDTIME_STR, js.date(info.scheduledTime));
  obj.set(js, CRON_STR, js.str(info.cron));
  return obj;
}

jsg::JsValue ToJs(jsg::Lock& js, const tracing::AlarmEventInfo& info, StringCache& cache) {
  auto obj = js.obj();
  obj.set(js, TYPE_STR, cache.get(js, ALARM_STR));
  obj.set(js, SCHEDULEDTIME_STR, js.date(info.scheduledTime));
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

  auto names = KJ_MAP(trace, info.traces) -> jsg::JsValue {
    KJ_IF_SOME(name, trace.scriptName) {
      return js.str(name);
    }
    return js.null();
  };
  obj.set(js, TRACES_STR, js.arr(names));

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
    auto vals = KJ_MAP(tag, tags) -> jsg::JsValue { return js.str(tag); };
    obj.set(js, SCRIPTTAGS_STR, js.arr(vals));
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
  KJ_IF_SOME(op, spanOpen.operationName) {
    obj.set(js, OP_STR, js.str(op));
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
    KJ_CASE_ONEOF(mark, tracing::Mark) {
      KJ_SWITCH_ONEOF(mark) {
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
    }
  }

  return obj;
}

// See the documentation for the identically named class in worker-rpc.c++ for details.
// TODO(cleanup): Combine this and the worker-rpc.c++ class into a single utility.
class ServerTopLevelMembrane final: public capnp::MembranePolicy, public kj::Refcounted {
 public:
  explicit ServerTopLevelMembrane(kj::Own<kj::PromiseFulfiller<void>> doneFulfiller)
      : doneFulfiller(kj::mv(doneFulfiller)) {}
  ~ServerTopLevelMembrane() noexcept(false) {
    KJ_IF_SOME(f, doneFulfiller) {
      f->reject(
          KJ_EXCEPTION(DISCONNECTED, "Tail stream session canceled without handling any events."));
    }
  }

  kj::Maybe<capnp::Capability::Client> inboundCall(
      uint64_t interfaceId, uint16_t methodId, capnp::Capability::Client target) override {
    auto f = kj::mv(JSG_REQUIRE_NONNULL(
        doneFulfiller, Error, "Only one tailStream method call is allowed on this object."));
    doneFulfiller = kj::none;
    return capnp::membrane(kj::mv(target), kj::refcounted<CompletionMembrane>(kj::mv(f)));
  }

  kj::Maybe<capnp::Capability::Client> outboundCall(
      uint64_t interfaceId, uint16_t methodId, capnp::Capability::Client target) override {
    KJ_FAIL_ASSERT("ServerTopLevelMembrane shouldn't have outgoing capabilities");
  }

  kj::Own<MembranePolicy> addRef() override {
    return kj::addRef(*this);
  }

 private:
  kj::Maybe<kj::Own<kj::PromiseFulfiller<void>>> doneFulfiller;
};

class TailStreamTargetBase: public rpc::TailStreamTarget::Server {
 public:
  TailStreamTargetBase(IoContext& ioContext): weakIoContext(ioContext.getWeakRef()) {}
  KJ_DISALLOW_COPY_AND_MOVE(TailStreamTargetBase);
  virtual ~TailStreamTargetBase() = default;

  virtual kj::Promise<void> runImpl(Worker::Lock& lock,
      IoContext& ioContext,
      kj::ArrayPtr<tracing::TailEvent> events,
      rpc::TailStreamTarget::TailStreamResults::Builder results) = 0;

  kj::Promise<void> report(ReportContext reportContext) override {
    IoContext& ioContext = JSG_REQUIRE_NONNULL(weakIoContext->tryGet(), Error,
        "The destination object for this tail session no longer exists.");

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
      auto result = runImpl(lock, ioContext, events.releaseAsArray(), reportContext.initResults());

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
  kj::Own<IoContext::WeakRef> weakIoContext;
};

// Returns the name of the handler function for this type of event.
kj::Maybe<kj::StringPtr> getHandlerName(const tracing::TailEvent& event) {
  KJ_SWITCH_ONEOF(event.event) {
    KJ_CASE_ONEOF(_, tracing::Onset) {
      return ONSET_STR;
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
    KJ_CASE_ONEOF(mark, tracing::Mark) {
      KJ_SWITCH_ONEOF(mark) {
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
        KJ_CASE_ONEOF(_, kj::Array<Attribute>) {
          return ATTRIBUTE_STR;
        }
      }
    }
  }
  return kj::none;
}

// The TailStreamEntrypoint class handles the initial onset event and the determination
// of whether additional events should be handled in this stream. If a handler is
// returned then a capability to access the TailStreamHandler is returned. This class
// takes over and handles the remaining events in the stream.
class TailStreamHandler final: public TailStreamTargetBase {
 public:
  TailStreamHandler(IoContext& ioContext, jsg::JsRef<jsg::JsValue> handler)
      : TailStreamTargetBase(ioContext),
        handler(kj::mv(handler)) {}

  kj::Promise<void> runImpl(Worker::Lock& lock,
      IoContext& ioContext,
      kj::ArrayPtr<tracing::TailEvent> events,
      rpc::TailStreamTarget::TailStreamResults::Builder results) {
    jsg::Lock& js = lock;

    if (events.size() == 0) return kj::READY_NOW;

    // Take the received set of events and dispatch them to the correct handler.

    v8::Local<v8::Value> h = handler.getHandle(js);
    v8::LocalVector<v8::Value> returnValues(js.v8Isolate);
    StringCache stringCache;

    if (h->IsFunction()) {
      // If the handler is a function, then we'll just pass all of the events to that
      // function. If the function returns a promise and there are multiple events we
      // will not wait for each promise to resolve before calling the next iteration.
      // But we will wait for all promises to settle before returning the resolved
      // kj promise.
      auto fn = h.As<v8::Function>();
      for (auto& event: events) {
        v8::Local<v8::Value> eventObj = ToJs(js, event, stringCache);
        returnValues.push_back(jsg::check(fn->Call(js.v8Context(), h, 1, &eventObj)));
      }
    } else {
      // If the handler is an object, then we need to know what kind of events
      // we have and look for a specific handler function for each.
      KJ_ASSERT(h->IsObject());
      jsg::JsObject obj = jsg::JsObject(h.As<v8::Object>());
      for (auto& event: events) {
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
      return ioContext.awaitJs(js, kj::mv(p));
    }
    return kj::READY_NOW;
  }

 private:
  jsg::JsRef<jsg::JsValue> handler;
};

// The TailStreamEndpoint class handles the initial onset event and the determination
// or whether additional events should be handled in this stream.
class TailStreamEntrypoint final: public TailStreamTargetBase {
 public:
  TailStreamEntrypoint(IoContext& ioContext, Frankenvalue props)
      : TailStreamTargetBase(ioContext),
        props(kj::mv(props)) {}

  kj::Promise<void> runImpl(Worker::Lock& lock,
      IoContext& ioContext,
      kj::ArrayPtr<tracing::TailEvent> events,
      rpc::TailStreamTarget::TailStreamResults::Builder results) {
    jsg::Lock& js = lock;
    // There should be only a single onset event in this one.
    KJ_ASSERT(events.size() == 1 && events[0].event.is<tracing::Onset>(),
        "Expected only a single onset event");
    auto& event = events[0];

    auto handler = KJ_REQUIRE_NONNULL(
        lock.getExportedHandler("default"_kj, kj::mv(props), ioContext.getActor()),
        "Failed to get handler to worker.");
    StringCache stringCache;

    auto target = jsg::JsObject(handler->self.getHandle(lock));
    v8::Local<v8::Value> maybeFn = target.get(lock, "tailStream"_kj);
    if (!maybeFn->IsFunction()) {
      // If there's no actual tailStream handler we will emit a warning for the user
      // then immediately return.
      ioContext.logWarningOnce("A worker configured to act as a streaming tail worker does "
                               "not export a tailStream() handler.");
      return kj::READY_NOW;
    }

    v8::Local<v8::Function> fn = maybeFn.As<v8::Function>();

    v8::Local<v8::Value> obj = ToJs(js, event, stringCache);
    auto result = jsg::check(fn->Call(js.v8Context(), target, 1, &obj));

    return ioContext.awaitJs(js,
        js.toPromise(result).then(js,
            ioContext.addFunctor(
                [results = kj::mv(results), &ioContext](jsg::Lock& js, jsg::Value value) mutable {
      // The value here can be one of a function, an object, or undefined.
      // Any value other than these will result in a warning but will otherwise
      // be treated like undefined.
      //
      // If a function or object is returned, we will pass a new capability
      // back to the caller that can be used to keep pushing events.
      //
      auto handle = value.getHandle(js);
      if (handle->IsFunction() || handle->IsObject()) {
        // Sweet! We'll take the handle and pass it off to a new TailStreamHandler
        // that we will use to initialize a new capability to return to the user.
        results.setPipeline(rpc::TailStreamTarget::Client(
            kj::heap<TailStreamHandler>(ioContext, jsg::JsRef(js, jsg::JsValue(handle)))));
        return;
      }

      if (!handle->IsUndefined()) {
        ioContext.logWarningOnce(
            "tailStream() handler returned an unusable value. "
            "The tailStream() handler is expected to return either a function, an "
            "object, or undefined.");
      }
    }),
            ioContext.addFunctor(
                [](jsg::Lock& js, jsg::Value&& error) { js.throwException(kj::mv(error)); })));
  }

 private:
  Frankenvalue props;
};
}  // namespace

kj::Promise<WorkerInterface::CustomEvent::Result> TailStreamCustomEventImpl::run(
    kj::Own<IoContext::IncomingRequest> incomingRequest,
    kj::Maybe<kj::StringPtr> entrypointName,
    Frankenvalue props,
    kj::TaskSet& waitUntilTasks) {
  IoContext& ioContext = incomingRequest->getContext();
  incomingRequest->delivered();

  auto [donePromise, doneFulfiller] = kj::newPromiseAndFulfiller<void>();
  capFulfiller->fulfill(capnp::membrane(kj::heap<TailStreamEntrypoint>(ioContext, kj::mv(props)),
      kj::refcounted<ServerTopLevelMembrane>(kj::mv(doneFulfiller))));

  KJ_DEFER({
    // waitUntil() should allow extending execution on the server side even when the client
    // disconnects.
    waitUntilTasks.add(incomingRequest->drain().attach(kj::mv(incomingRequest)));
  });

  // `donePromise` resolves once there are no longer any capabilities pointing between the client
  // and server as part of this session.
  co_await donePromise.exclusiveJoin(ioContext.onAbort());

  co_return WorkerInterface::CustomEvent::Result{.outcome = EventOutcome::OK};
}

kj::Promise<WorkerInterface::CustomEvent::Result> TailStreamCustomEventImpl::sendRpc(
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

tracing::TailStreamWriter::TailStreamWriter(Reporter reporter): state(State(kj::mv(reporter))) {}

void tracing::TailStreamWriter::report(IoContext& ioContext, TailEvent::Event&& event) {
  auto& s = KJ_UNWRAP_OR_RETURN(state);
  bool ending = event.tryGet<tracing::Outcome>() != kj::none ||
      event.tryGet<tracing::Hibernate>() != kj::none;
  KJ_DEFER({
    if (ending) state = kj::none;
  });
  if (event.tryGet<tracing::Onset>() != kj::none) {
    KJ_ASSERT(!s.onsetSeen, "Tail stream onset already provided");
    s.onsetSeen = true;
  }
  tracing::TailEvent tailEvent(
      ioContext.getInvocationSpanContext(), ioContext.now(), s.sequence++, kj::mv(event));

  // If the reporter returns false, then we will treat it as a close signal.
  ending = !s.reporter(ioContext, kj::mv(tailEvent));
}

}  // namespace workerd::tracing
