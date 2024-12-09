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
jsg::JsValue ToJs(jsg::Lock& js, const tracing::Attribute& attribute) {
  auto obj = js.obj();
  obj.set(js, "type"_kj, js.str("attribute"_kj));
  obj.set(js, "name"_kj, js.str(attribute.name));

  auto ToJs = [](jsg::Lock& js, const tracing::Attribute::Value& value) -> jsg::JsValue {
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
  };

  if (attribute.value.size() == 1) {
    obj.set(js, "value"_kj, ToJs(js, attribute.value[0]));
  } else {
    auto values = KJ_MAP(val, attribute.value) { return ToJs(js, val); };
    obj.set(js, "value"_kj, js.arr(values));
  }

  return obj;
}

jsg::JsValue ToJs(jsg::Lock& js, kj::ArrayPtr<const tracing::Attribute> attributes) {
  auto attrs = KJ_MAP(attr, attributes) { return ToJs(js, attr); };
  return js.arr(attrs);
}

jsg::JsValue ToJs(jsg::Lock& js, const tracing::FetchResponseInfo& info) {
  auto obj = js.obj();
  obj.set(js, "type"_kj, js.str("fetch"_kj));
  obj.set(js, "statusCode"_kj, js.num(info.statusCode));
  return obj;
}

jsg::JsValue ToJs(jsg::Lock& js, const tracing::FetchEventInfo& info) {
  auto obj = js.obj();
  obj.set(js, "type"_kj, js.str("fetch"_kj));
  obj.set(js, "method"_kj, js.str(kj::str(info.method)));
  obj.set(js, "url"_kj, js.str(info.url));
  obj.set(js, "cfJson"_kj, js.str(info.cfJson));

  auto ToJs = [](jsg::Lock& js, const tracing::FetchEventInfo::Header& header) {
    auto obj = js.obj();
    obj.set(js, "name"_kj, js.str(header.name));
    obj.set(js, "value"_kj, js.str(header.value));
    return obj;
  };

  auto headers = KJ_MAP(header, info.headers) -> jsg::JsValue { return ToJs(js, header); };
  obj.set(js, "headers", js.arr(headers));
  return obj;
}

jsg::JsValue ToJs(jsg::Lock& js, const tracing::JsRpcEventInfo& info) {
  auto obj = js.obj();
  obj.set(js, "type"_kj, js.str("jsrpc"_kj));
  obj.set(js, "methodName"_kj, js.str(info.methodName));
  return obj;
}

jsg::JsValue ToJs(jsg::Lock& js, const tracing::ScheduledEventInfo& info) {
  auto obj = js.obj();
  obj.set(js, "type"_kj, js.str("scheduled"_kj));
  obj.set(js, "scheduledTime"_kj, js.date(info.scheduledTime));
  obj.set(js, "cron"_kj, js.str(info.cron));
  return obj;
}

jsg::JsValue ToJs(jsg::Lock& js, const tracing::AlarmEventInfo& info) {
  auto obj = js.obj();
  obj.set(js, "type"_kj, js.str("alarm"_kj));
  obj.set(js, "scheduledTime"_kj, js.date(info.scheduledTime));
  return obj;
}

jsg::JsValue ToJs(jsg::Lock& js, const tracing::QueueEventInfo& info) {
  auto obj = js.obj();
  obj.set(js, "type"_kj, js.str("queue"_kj));
  obj.set(js, "queueName"_kj, js.str(info.queueName));
  obj.set(js, "batchSize"_kj, js.num(info.batchSize));
  return obj;
}

jsg::JsValue ToJs(jsg::Lock& js, const tracing::EmailEventInfo& info) {
  auto obj = js.obj();
  obj.set(js, "type"_kj, js.str("email"_kj));
  obj.set(js, "mailFrom"_kj, js.str(info.mailFrom));
  obj.set(js, "rcptTo"_kj, js.str(info.rcptTo));
  obj.set(js, "rawSize"_kj, js.num(info.rawSize));
  return obj;
}

jsg::JsValue ToJs(jsg::Lock& js, const tracing::TraceEventInfo& info) {
  auto obj = js.obj();
  obj.set(js, "type"_kj, js.str("trace"_kj));

  auto names = KJ_MAP(trace, info.traces) -> jsg::JsValue {
    KJ_IF_SOME(name, trace.scriptName) {
      return js.str(name);
    }
    return js.null();
  };
  obj.set(js, "traces"_kj, js.arr(names));

  return obj;
}

jsg::JsValue ToJs(jsg::Lock& js, const tracing::HibernatableWebSocketEventInfo& info) {
  auto obj = js.obj();
  obj.set(js, "type"_kj, js.str("hibernatableWebSocket"_kj));

  KJ_SWITCH_ONEOF(info.type) {
    KJ_CASE_ONEOF(message, tracing::HibernatableWebSocketEventInfo::Message) {
      auto mobj = js.obj();
      mobj.set(js, "type"_kj, js.str("message"_kj));
      obj.set(js, "info"_kj, mobj);
    }
    KJ_CASE_ONEOF(error, tracing::HibernatableWebSocketEventInfo::Error) {
      auto mobj = js.obj();
      mobj.set(js, "type"_kj, js.str("error"_kj));
      obj.set(js, "info"_kj, mobj);
    }
    KJ_CASE_ONEOF(close, tracing::HibernatableWebSocketEventInfo::Close) {
      auto mobj = js.obj();
      mobj.set(js, "type"_kj, js.str("close"_kj));
      mobj.set(js, "code"_kj, js.num(close.code));
      mobj.set(js, "wasClean"_kj, js.boolean(close.wasClean));
      obj.set(js, "info"_kj, mobj);
    }
  }

  return obj;
}

jsg::JsValue ToJs(jsg::Lock& js, const tracing::Resume& info) {
  auto obj = js.obj();
  obj.set(js, "type"_kj, js.str("resume"_kj));

  KJ_IF_SOME(attachment, info.attachment) {
    jsg::Serializer::Released data{
      .data = kj::heapArray<kj::byte>(attachment),
    };
    jsg::Deserializer deser(js, data);
    obj.set(js, "attachment"_kj, deser.readValue(js));
  }

  return obj;
}

jsg::JsValue ToJs(jsg::Lock& js, const tracing::CustomEventInfo& info) {
  auto obj = js.obj();
  obj.set(js, "type"_kj, js.str("custom"_kj));
  return obj;
}

jsg::JsValue ToJs(jsg::Lock& js, const EventOutcome& outcome) {
  switch (outcome) {
    case EventOutcome::OK:
      return js.str("ok"_kj);
    case EventOutcome::CANCELED:
      return js.str("canceled"_kj);
    case EventOutcome::EXCEPTION:
      return js.str("exception"_kj);
    case EventOutcome::KILL_SWITCH:
      return js.str("kilSwitch"_kj);
    case EventOutcome::DAEMON_DOWN:
      return js.str("daemonDown"_kj);
    case EventOutcome::EXCEEDED_CPU:
      return js.str("exceededCpu"_kj);
    case EventOutcome::EXCEEDED_MEMORY:
      return js.str("exceededMemory"_kj);
    case EventOutcome::LOAD_SHED:
      return js.str("loadShed"_kj);
    case EventOutcome::RESPONSE_STREAM_DISCONNECTED:
      return js.str("responseStreamDisconnected"_kj);
    case EventOutcome::SCRIPT_NOT_FOUND:
      return js.str("scriptNotFound"_kj);
    case EventOutcome::UNKNOWN:
      return js.str("unknown"_kj);
  }
  KJ_UNREACHABLE;
}

jsg::JsValue ToJs(jsg::Lock& js, const tracing::Onset& onset) {
  auto obj = js.obj();
  obj.set(js, "type"_kj, js.str("onset"_kj));

  KJ_IF_SOME(ns, onset.workerInfo.dispatchNamespace) {
    obj.set(js, "dispatchNamespace"_kj, js.str(ns));
  }
  KJ_IF_SOME(entrypoint, onset.workerInfo.entrypoint) {
    obj.set(js, "entrypoint"_kj, js.str(entrypoint));
  }
  KJ_IF_SOME(name, onset.workerInfo.scriptName) {
    obj.set(js, "scriptName"_kj, js.str(name));
  }
  KJ_IF_SOME(tags, onset.workerInfo.scriptTags) {
    auto vals = KJ_MAP(tag, tags) -> jsg::JsValue { return js.str(tag); };
    obj.set(js, "scriptTags"_kj, js.arr(vals));
  }
  KJ_IF_SOME(version, onset.workerInfo.scriptVersion) {
    auto vobj = js.obj();
    auto id = version->getId();
    KJ_IF_SOME(uuid, UUID::fromUpperLower(id.getUpper(), id.getLower())) {
      vobj.set(js, "id"_kj, js.str(uuid.toString()));
    }
    if (version->hasTag()) {
      vobj.set(js, "tag"_kj, js.str(version->getTag()));
    }
    if (version->hasMessage()) {
      vobj.set(js, "message"_kj, js.str(version->getMessage()));
    }
    obj.set(js, "scriptVersion", vobj);
  }

  KJ_IF_SOME(trigger, onset.trigger) {
    auto tobj = js.obj();
    tobj.set(js, "traceId"_kj, js.str(trigger.traceId.toGoString()));
    tobj.set(js, "invocationId"_kj, js.str(trigger.invocationId.toGoString()));
    tobj.set(js, "spanId"_kj, js.str(trigger.spanId.toGoString()));
    obj.set(js, "trigger", tobj);
  }

  KJ_SWITCH_ONEOF(onset.info) {
    KJ_CASE_ONEOF(fetch, tracing::FetchEventInfo) {
      obj.set(js, "info"_kj, ToJs(js, fetch));
    }
    KJ_CASE_ONEOF(jsrpc, tracing::JsRpcEventInfo) {
      obj.set(js, "info"_kj, ToJs(js, jsrpc));
    }
    KJ_CASE_ONEOF(scheduled, tracing::ScheduledEventInfo) {
      obj.set(js, "info"_kj, ToJs(js, scheduled));
    }
    KJ_CASE_ONEOF(alarm, tracing::AlarmEventInfo) {
      obj.set(js, "info"_kj, ToJs(js, alarm));
    }
    KJ_CASE_ONEOF(queue, tracing::QueueEventInfo) {
      obj.set(js, "info"_kj, ToJs(js, queue));
    }
    KJ_CASE_ONEOF(email, tracing::EmailEventInfo) {
      obj.set(js, "info"_kj, ToJs(js, email));
    }
    KJ_CASE_ONEOF(trace, tracing::TraceEventInfo) {
      obj.set(js, "info"_kj, ToJs(js, trace));
    }
    KJ_CASE_ONEOF(hws, tracing::HibernatableWebSocketEventInfo) {
      obj.set(js, "info"_kj, ToJs(js, hws));
    }
    KJ_CASE_ONEOF(resume, tracing::Resume) {
      obj.set(js, "info"_kj, ToJs(js, resume));
    }
    KJ_CASE_ONEOF(custom, tracing::CustomEventInfo) {
      obj.set(js, "info"_kj, ToJs(js, custom));
    }
  }

  return obj;
}

jsg::JsValue ToJs(jsg::Lock& js, const tracing::Outcome& outcome) {
  auto obj = js.obj();
  obj.set(js, "type"_kj, js.str("outcome"_kj));
  obj.set(js, "outcome"_kj, ToJs(js, outcome.outcome));

  double cpuTime = outcome.cpuTime / kj::MILLISECONDS;
  double wallTime = outcome.wallTime / kj::MILLISECONDS;

  obj.set(js, "cpuTime"_kj, js.num(cpuTime));
  obj.set(js, "wallTime"_kj, js.num(wallTime));

  return obj;
}

jsg::JsValue ToJs(jsg::Lock& js, const tracing::Hibernate& hibernate) {
  auto obj = js.obj();
  obj.set(js, "type"_kj, js.str("hibernate"_kj));
  return obj;
}

jsg::JsValue ToJs(jsg::Lock& js, const tracing::SpanOpen& spanOpen) {
  auto obj = js.obj();
  obj.set(js, "type"_kj, js.str("spanOpen"_kj));
  KJ_IF_SOME(op, spanOpen.operationName) {
    obj.set(js, "op"_kj, js.str(op));
  }
  KJ_IF_SOME(info, spanOpen.info) {
    KJ_SWITCH_ONEOF(info) {
      KJ_CASE_ONEOF(fetch, tracing::FetchEventInfo) {
        obj.set(js, "info"_kj, ToJs(js, fetch));
      }
      KJ_CASE_ONEOF(jsrpc, tracing::JsRpcEventInfo) {
        obj.set(js, "info"_kj, ToJs(js, jsrpc));
      }
      KJ_CASE_ONEOF(custom, tracing::CustomInfo) {
        obj.set(js, "info"_kj, ToJs(js, custom.asPtr()));
      }
    }
  }
  return obj;
}

jsg::JsValue ToJs(jsg::Lock& js, const tracing::SpanClose& spanClose) {
  auto obj = js.obj();
  obj.set(js, "type"_kj, js.str("spanClose"_kj));
  obj.set(js, "outcome"_kj, ToJs(js, spanClose.outcome));
  return obj;
}

jsg::JsValue ToJs(jsg::Lock& js, const tracing::DiagnosticChannelEvent& dce) {
  auto obj = js.obj();
  obj.set(js, "type"_kj, js.str("diagnosticChannel"_kj));
  obj.set(js, "channel"_kj, js.str(dce.channel));
  jsg::Serializer::Released released{
    .data = kj::heapArray<kj::byte>(dce.message),
  };
  jsg::Deserializer deser(js, released);
  obj.set(js, "message"_kj, deser.readValue(js));
  return obj;
}

jsg::JsValue ToJs(jsg::Lock& js, const tracing::Exception& ex) {
  auto obj = js.obj();
  obj.set(js, "type"_kj, js.str("exception"_kj));
  obj.set(js, "name"_kj, js.str(ex.name));
  obj.set(js, "message"_kj, js.str(ex.message));
  KJ_IF_SOME(stack, ex.stack) {
    obj.set(js, "stack"_kj, js.str(stack));
  }
  return obj;
}

jsg::JsValue ToJs(jsg::Lock& js, const LogLevel& level) {
  switch (level) {
    case LogLevel::DEBUG_:
      return js.str("debug"_kj);
    case LogLevel::ERROR:
      return js.str("error"_kj);
    case LogLevel::INFO:
      return js.str("info"_kj);
    case LogLevel::LOG:
      return js.str("log"_kj);
    case LogLevel::WARN:
      return js.str("warn"_kj);
  }
  KJ_UNREACHABLE;
}

jsg::JsValue ToJs(jsg::Lock& js, const tracing::Log& log) {
  auto obj = js.obj();
  obj.set(js, "type"_kj, js.str("log"_kj));
  obj.set(js, "level", ToJs(js, log.logLevel));
  obj.set(js, "message", js.str(log.message));
  return obj;
}

jsg::JsValue ToJs(jsg::Lock& js, const tracing::Return& ret) {
  auto obj = js.obj();
  obj.set(js, "type"_kj, js.str("return"_kj));

  KJ_IF_SOME(info, ret.info) {
    KJ_SWITCH_ONEOF(info) {
      KJ_CASE_ONEOF(fetch, tracing::FetchResponseInfo) {
        obj.set(js, "info"_kj, ToJs(js, fetch));
      }
      KJ_CASE_ONEOF(custom, tracing::CustomInfo) {
        obj.set(js, "info"_kj, ToJs(js, custom.asPtr()));
      }
    }
  }

  return obj;
}

jsg::JsValue ToJs(jsg::Lock& js, const tracing::Link& link) {
  auto obj = js.obj();
  obj.set(js, "type"_kj, js.str("link"_kj));
  KJ_IF_SOME(label, link.label) {
    obj.set(js, "label"_kj, js.str(label));
  }
  obj.set(js, "traceId"_kj, js.str(link.traceId.toGoString()));
  obj.set(js, "invocationId"_kj, js.str(link.invocationId.toGoString()));
  obj.set(js, "spanId"_kj, js.str(link.spanId.toGoString()));
  return obj;
}

jsg::JsValue ToJs(jsg::Lock& js, const tracing::TailEvent& event) {
  auto obj = js.obj();
  obj.set(js, "traceId"_kj, js.str(event.traceId.toGoString()));
  obj.set(js, "invocationId"_kj, js.str(event.invocationId.toGoString()));
  obj.set(js, "spanId"_kj, js.str(event.spanId.toGoString()));
  obj.set(js, "timestamp"_kj, js.date(event.timestamp));
  obj.set(js, "sequence"_kj, js.num(event.sequence));

  KJ_SWITCH_ONEOF(event.event) {
    KJ_CASE_ONEOF(onset, tracing::Onset) {
      obj.set(js, "event"_kj, ToJs(js, onset));
    }
    KJ_CASE_ONEOF(outcome, tracing::Outcome) {
      obj.set(js, "event"_kj, ToJs(js, outcome));
    }
    KJ_CASE_ONEOF(hibernate, tracing::Hibernate) {
      obj.set(js, "event"_kj, ToJs(js, hibernate));
    }
    KJ_CASE_ONEOF(spanOpen, tracing::SpanOpen) {
      obj.set(js, "event"_kj, ToJs(js, spanOpen));
    }
    KJ_CASE_ONEOF(spanClose, tracing::SpanClose) {
      obj.set(js, "event"_kj, ToJs(js, spanClose));
    }
    KJ_CASE_ONEOF(mark, tracing::Mark) {
      KJ_SWITCH_ONEOF(mark) {
        KJ_CASE_ONEOF(de, tracing::DiagnosticChannelEvent) {
          obj.set(js, "event"_kj, ToJs(js, de));
        }
        KJ_CASE_ONEOF(ex, tracing::Exception) {
          obj.set(js, "event"_kj, ToJs(js, ex));
        }
        KJ_CASE_ONEOF(log, tracing::Log) {
          obj.set(js, "event"_kj, ToJs(js, log));
        }
        KJ_CASE_ONEOF(ret, tracing::Return) {
          obj.set(js, "event"_kj, ToJs(js, ret));
        }
        KJ_CASE_ONEOF(link, tracing::Link) {
          obj.set(js, "event"_kj, ToJs(js, link));
        }
        KJ_CASE_ONEOF(attrs, kj::Array<tracing::Attribute>) {
          obj.set(js, "event"_kj, ToJs(js, attrs));
        }
      }
    }
  }

  return obj;
}

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

kj::Maybe<kj::StringPtr> getHandlerName(const tracing::TailEvent& event) {
  KJ_SWITCH_ONEOF(event.event) {
    KJ_CASE_ONEOF(_, tracing::Onset) {
      return "onset"_kj;
    }
    KJ_CASE_ONEOF(_, tracing::Outcome) {
      return "outcome"_kj;
    }
    KJ_CASE_ONEOF(_, tracing::Hibernate) {
      return "hibernate"_kj;
    }
    KJ_CASE_ONEOF(_, tracing::SpanOpen) {
      return "spanOpen"_kj;
    }
    KJ_CASE_ONEOF(_, tracing::SpanClose) {
      return "spanClose"_kj;
    }
    KJ_CASE_ONEOF(mark, tracing::Mark) {
      KJ_SWITCH_ONEOF(mark) {
        KJ_CASE_ONEOF(_, tracing::DiagnosticChannelEvent) {
          return "diagnosticChannel"_kj;
        }
        KJ_CASE_ONEOF(_, tracing::Exception) {
          return "exception"_kj;
        }
        KJ_CASE_ONEOF(_, tracing::Log) {
          return "log"_kj;
        }
        KJ_CASE_ONEOF(_, tracing::Return) {
          return "return"_kj;
        }
        KJ_CASE_ONEOF(_, tracing::Link) {
          return "link"_kj;
        }
        KJ_CASE_ONEOF(_, kj::Array<Attribute>) {
          return "attribute"_kj;
        }
      }
    }
  }
  return kj::none;
}

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

    v8::Local<v8::Value> h = handler.getHandle(js);
    v8::LocalVector<v8::Value> returnValues(js.v8Isolate, events.size());

    if (h->IsFunction()) {
      // If the handler is a function, then we'll just pass all of the events to that
      // function. If the function returns a promise and there are multiple events we
      // will not wait for each promise to resolve before calling the next iteration.
      // But we will wait for all promises to settle before returning the resolved
      // kj promise.
      auto fn = h.As<v8::Function>();
      for (auto& event: events) {
        v8::Local<v8::Value> eventObj = ToJs(js, event);
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
            v8::Local<v8::Value> eventObj = ToJs(js, event);
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

class TailStreamEntrypoint final: public TailStreamTargetBase {
 public:
  TailStreamEntrypoint(IoContext& ioContext): TailStreamTargetBase(ioContext) {}

  kj::Promise<void> runImpl(Worker::Lock& lock,
      IoContext& ioContext,
      kj::ArrayPtr<tracing::TailEvent> events,
      rpc::TailStreamTarget::TailStreamResults::Builder results) {
    jsg::Lock& js = lock;
    // There should be only a single event in this one.
    KJ_ASSERT(events.size() == 1 && events[0].event.is<tracing::Onset>(),
        "Expected only a single onset event");
    auto& event = events[0];

    auto handler = KJ_REQUIRE_NONNULL(lock.getExportedHandler("default"_kj, ioContext.getActor()),
        "Failed to get handler to worker.");

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

    v8::Local<v8::Value> obj = ToJs(js, event);
    auto result = jsg::check(fn->Call(js.v8Context(), target, 1, &obj));

    return ioContext.awaitJs(js,
        js.toPromise(result).then(js,
            ioContext.addFunctor([&results, &ioContext](jsg::Lock& js, jsg::Value value) {
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
};
}  // namespace

kj::Promise<WorkerInterface::CustomEvent::Result> TailStreamCustomEventImpl::run(
    kj::Own<IoContext::IncomingRequest> incomingRequest,
    kj::Maybe<kj::StringPtr> entrypointName,
    kj::TaskSet& waitUntilTasks) {
  IoContext& ioContext = incomingRequest->getContext();
  incomingRequest->delivered();

  auto [donePromise, doneFulfiller] = kj::newPromiseAndFulfiller<void>();
  capFulfiller->fulfill(capnp::membrane(kj::heap<TailStreamEntrypoint>(ioContext),
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
