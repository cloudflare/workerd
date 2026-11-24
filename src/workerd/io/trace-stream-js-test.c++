// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Tests the JavaScript values a streaming tail worker receives for each tail event. Each test runs
// a real tail worker, sends it batches of events through TailStreamTarget::report(), as a source
// worker's TailStreamWriter does, and compares what the worker received, written out by `dump()`
// below, with a section of trace-stream-js-test.golden.txt. A test whose output differs writes it
// to <section>.txt in $TEST_UNDECLARED_OUTPUTS_DIR, from which the section can be updated.

#include <workerd/io/trace-stream-js-test.golden.embed.h>
#include <workerd/io/trace-stream.h>
#include <workerd/io/worker-interface.capnp.h>
#include <workerd/jsg/ser.h>
#include <workerd/tests/test-fixture.h>

#include <kj/filesystem.h>
#include <kj/test.h>

namespace workerd::tracing {
namespace {

// Records what tailStream() and its handler receive, in `globalThis.received`, one line each.
//
// `dump()` writes a value out with everything a tail worker could observe: key order, the type
// of each primitive, and the prototype of each object. tailStream() returns a handler chosen by
// the onset's entrypoint: a function, or a Proxy that records each handler name looked up on it.
// It returns undefined, ending the session, if there is no entrypoint.
constexpr kj::StringPtr TAIL_WORKER = R"SCRIPT(
  function dump(value) {
    if (value === null) return "null";
    switch (typeof value) {
      case "undefined": return "undefined";
      case "string": return JSON.stringify(value);
      case "number": return Object.is(value, -0) ? "-0" : String(value);
      case "bigint": return `${value}n`;
      case "boolean": return String(value);
    }
    if (value instanceof Date) return `Date(${value.getTime()})`;
    if (Array.isArray(value)) return `[${value.map(dump).join(", ")}]`;
    if (value instanceof Map) {
      return `Map{${[...value].map(([k, v]) => `${dump(k)} => ${dump(v)}`).join(", ")}}`;
    }
    if (value instanceof Set) return `Set{${[...value].map(dump).join(", ")}}`;
    if (value instanceof RegExp) return `RegExp(${value})`;
    if (value instanceof Headers) return `Headers${dump([...value])}`;
    // Whether a DOMException's fields are its own properties depends on how it is implemented.
    if (value instanceof DOMException) {
      return `DOMException{name: ${dump(value.name)}, message: ${dump(value.message)}, ` +
          `code: ${dump(value.code)}, stack: ${dump(value.stack)}}`;
    }
    const proto = Object.getPrototypeOf(value);
    const tag = proto === Object.prototype ? "" : proto === null ? "<null> " : `${proto.constructor.name} `;
    const keys = Reflect.ownKeys(value).map((key) => {
      const descriptor = Object.getOwnPropertyDescriptor(value, key);
      const flags = (descriptor.writable === false ? "!w" : "") +
          (descriptor.enumerable ? "" : "!e") + (descriptor.configurable ? "" : "!c");
      return `${String(key)}${flags ? `(${flags})` : ""}: ${dump(value[key])}`;
    });
    return `${tag}{${keys.join(", ")}}`;
  }

  globalThis.received = [];
  // With setters defined on Object.prototype (see SETTERS), each line also lists the properties
  // set through them since the previous line.
  const record = (line) => globalThis.received.push(
      globalThis.sets ? `${line}\n  sets: ${globalThis.sets.splice(0).join(" ")}` : line);

  export default {
    tailStream(onset) {
      record(`onset ${dump(onset)}`);
      switch (onset.event.entrypoint) {
        case "function":
          return (event) => record(`function ${dump(event)}`);
        case "object":
          return new Proxy({}, {
            get(target, name) {
              record(`get ${String(name)}`);
              // Resolving the promise of tailStream()'s result reads `then`.
              if (name === "then") return undefined;
              return (event) => record(`${String(name)} ${dump(event)}`);
            },
          });
      }
      return undefined;
    }
  };
)SCRIPT"_kj;

// Defines a setter on Object.prototype for each property name a tail event has, which records the
// name in `globalThis.sets` and defines the property on the object. Each property of an event is
// set as by an assignment, so a script sees the order in which they are set.
constexpr kj::StringPtr SETTERS = R"SCRIPT(
  globalThis.sets = [];
  for (const key of [
    "attributes", "batchSize", "cfJson", "channel", "code", "context", "count", "cpuTime", "cron",
    "diagnostic", "diagnosticsType", "dispatchNamespace", "droppedAttributesCount",
    "durableObjectId", "entrypoint", "errorInfo", "event", "executionModel", "headers", "id",
    "info", "invocationId", "level", "links", "mailFrom", "message", "method", "name", "outcome",
    "preview", "queueName", "rawSize", "rcptTo", "reason", "scheduledTime", "scriptName",
    "scriptTags", "scriptVersion", "sequence", "slug", "spanContext", "spanId", "stack", "status",
    "statusCode", "tag", "timestamp", "traceFlags", "traceId", "traces", "truncated", "type",
    "url", "value", "wallTime", "wasClean",
  ]) {
    // The descriptors have no prototype, which would give them the accessor named "value".
    Object.defineProperty(Object.prototype, key, {
      __proto__: null,
      set(value) {
        globalThis.sets.push(key);
        Object.defineProperty(this, key, {
          __proto__: null, value, writable: true, enumerable: true, configurable: true,
        });
      },
      configurable: true,
    });
  }
)SCRIPT"_kj;

struct ErrorHandler final: public kj::TaskSet::ErrorHandler {
  void taskFailed(kj::Exception&& exception) override {
    KJ_FAIL_EXPECT("waitUntil task failed", exception);
  }
};

using EventsBuilder = capnp::List<rpc::Trace::TailEvent>::Builder;

// One tail session with the tail worker of `fixture`.
class TailSession {
 public:
  KJ_DISALLOW_COPY_AND_MOVE(TailSession);

  explicit TailSession(TestFixture& fixture): fixture(fixture), waitUntilTasks(errorHandler) {
    auto event = kj::heap<TailStreamCustomEvent>();
    cap = event->getCap();
    auto context = fixture.newIoContext();
    done = event
               ->run(fixture.newUndeliveredIncomingRequest(*context), kj::none, kj::none,
                   Frankenvalue(), waitUntilTasks, false)
               .attach(kj::mv(event));
  }

  // Sends a batch of `count` events, which `build` fills in, and returns whether the tail worker
  // asked for no more.
  bool report(uint count, kj::FunctionParam<void(EventsBuilder)> build) {
    auto request = cap.reportRequest();
    build(request.initEvents(count));
    return request.send().wait(fixture.getWaitScope()).getStop();
  }

  // Waits for the session to end, and returns its outcome.
  EventOutcome finish() {
    auto outcome = done.wait(fixture.getWaitScope()).outcome;
    waitUntilTasks.onEmpty().wait(fixture.getWaitScope());
    return outcome;
  }

 private:
  TestFixture& fixture;
  ErrorHandler errorHandler;
  kj::TaskSet waitUntilTasks;
  rpc::TailStreamTarget::Client cap = nullptr;
  kj::Promise<WorkerInterface::CustomEvent::Result> done = nullptr;
};

TestFixture::SetupParams tailWorker() {
  return {.mainModuleSource = TAIL_WORKER};
}

// What the tail worker has received, one line each, since this was last called.
kj::String takeReceived(TestFixture& fixture) {
  kj::String result;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;
    auto global = jsg::JsObject(js.v8Context()->Global());
    auto received = KJ_ASSERT_NONNULL(global.get(js, "received"_kj).tryCast<jsg::JsArray>());
    kj::Vector<kj::String> lines;
    for (auto i: kj::zeroTo(received.size())) {
      lines.add(received.get(js, i).toString(js));
    }
    result = kj::strArray(lines, "\n");
    global.set(js, "received"_kj, js.arr());
  });
  return result;
}

// The section of the golden file headed `=== name`: the lines up to the next section, without
// the newline that ends the last one. None if there is no such section.
kj::Maybe<kj::ArrayPtr<const char>> goldenSection(kj::StringPtr name) {
  kj::StringPtr rest = TRACE_STREAM_JS_TEST_GOLDEN;
  auto header = kj::str("=== ", name, "\n");
  while (rest.size() > 0) {
    auto lineEnd = KJ_ASSERT_NONNULL(rest.findFirst('\n')) + 1;
    if (rest.first(lineEnd) == header) {
      auto body = rest.slice(lineEnd);
      size_t end = body.size();
      for (size_t i = 0; i < body.size();
           i = KJ_ASSERT_NONNULL(body.slice(i).findFirst('\n')) + i + 1) {
        if (body.slice(i).startsWith("=== ")) {
          end = i;
          break;
        }
      }
      KJ_ASSERT(end > 0 && body[end - 1] == '\n', "an empty golden section", name);
      return body.asArray().first(end - 1);
    }
    rest = rest.slice(lineEnd);
  }
  return kj::none;
}

// Expects `actual` to be the golden section `name`.
void expectGolden(kj::StringPtr name, kj::StringPtr actual) {
  auto expected = goldenSection(name);
  KJ_IF_SOME(e, expected) {
    if (e == actual.asArray()) return;
  }
  const char* outputs = getenv("TEST_UNDECLARED_OUTPUTS_DIR");
  if (outputs != nullptr) {
    auto fs = kj::newDiskFilesystem();
    auto path = fs->getCurrentPath().evalNative(outputs).append(kj::str(name, ".txt"));
    fs->getRoot()
        .openFile(
            path, kj::WriteMode::CREATE | kj::WriteMode::MODIFY | kj::WriteMode::CREATE_PARENT)
        ->writeAll(kj::str(actual, "\n"));
  }
  KJ_FAIL_EXPECT("output differs from the golden section", name, actual);
}

// Fills in the fields every event has: a span context with a span ID and trace flags, an
// invocation ID whose high half is set, and a timestamp with sub-millisecond nanoseconds.
void setCommon(rpc::Trace::TailEvent::Builder event, uint32_t sequence) {
  auto context = event.initSpanContext();
  auto traceId = context.initTraceId();
  traceId.setHigh(0x0123456789abcdef);
  traceId.setLow(0xfedcba9876543210);
  context.getInfo().setSpanId(0x00000000000000ab);
  context.initTraceFlags().getValue().setSet(1);
  auto invocationId = event.initInvocationId();
  invocationId.setHigh(0x1111111111111111);
  invocationId.setLow(0x2222222222222222);
  event.setTimestampNs(1'700'000'000'123'456'789);
  event.setSequence(sequence);
}

void setAttribute(rpc::Trace::Attribute::Builder attribute,
    kj::StringPtr name,
    kj::FunctionParam<void(capnp::List<rpc::TagValue>::Builder)> values,
    uint count) {
  attribute.setName(name);
  values(attribute.initValue(count));
}

// Attributes with one value of each type, two values, no values, and one value of each array
// type, whose second element is empty.
void setAttributes(capnp::List<rpc::Trace::Attribute>::Builder attributes) {
  setAttribute(attributes[0], "str", [](auto v) { v[0].setString("text"); }, 1);
  setAttribute(attributes[1], "bool", [](auto v) { v[0].setBool(true); }, 1);
  setAttribute(attributes[2], "int", [](auto v) { v[0].setInt64(-9'007'199'254'740'993); }, 1);
  setAttribute(attributes[3], "float", [](auto v) { v[0].setFloat64(-0.0); }, 1);
  setAttribute(attributes[4], "many", [](auto v) {
    v[0].setString("a");
    v[1].setInt64(2);
  }, 2);
  setAttribute(attributes[5], "none", [](auto) {}, 0);
  setAttribute(attributes[6], "strs", [](auto v) {
    auto list = v[0].initStringArray(2);
    list[0].setValue("x");
    list[1].setNone();
  }, 1);
  setAttribute(attributes[7], "bools", [](auto v) {
    auto list = v[0].initBoolArray(2);
    list[0].setValue(false);
    list[1].setNone();
  }, 1);
  setAttribute(attributes[8], "floats", [](auto v) {
    auto list = v[0].initFloat64Array(2);
    list[0].setValue(1.5);
    list[1].setNone();
  }, 1);
}

void setFetchInfo(rpc::Trace::FetchEventInfo::Builder fetch, kj::StringPtr cfJson) {
  fetch.setMethod(capnp::HttpMethod::POST);
  fetch.setUrl("https://example.com/path?q=1");
  fetch.setCfJson(cfJson);
  auto headers = fetch.initHeaders(2);
  headers[0].setName("content-type");
  headers[0].setValue("text/plain");
  headers[1].setName("x-custom");
  headers[1].setValue("vé");
}

// Starts a session whose onset has the given entrypoint, every optional worker field and a fetch
// event.
kj::Own<TailSession> startFull(TestFixture& fixture, kj::StringPtr entrypoint) {
  auto session = kj::heap<TailSession>(fixture);
  session->report(1, [&](EventsBuilder events) {
    auto event = events[0];
    setCommon(event, 0);
    auto onset = event.getEvent().initOnset();
    onset.setExecutionModel(rpc::Trace::ExecutionModel::DURABLE_OBJECT);
    onset.setScriptName("my-script");
    auto version = onset.initScriptVersion();
    version.getId().setUpper(0x0123456789abcdef);
    version.getId().setLower(0x0fedcba987654321);
    version.setTag("v1");
    version.setMessage("first");
    onset.setDispatchNamespace("my-namespace");
    onset.setScriptId("my-script-id");
    auto tags = onset.initScriptTags(2);
    tags.set(0, "a");
    tags.set(1, "b");
    onset.setEntryPoint(entrypoint);
    auto preview = onset.initPreview();
    preview.setId("preview-id");
    preview.setSlug("preview-slug");
    preview.setName("preview-name");
    onset.setDurableObjectId("do-id");
    setFetchInfo(onset.initInfo().initFetch(), R"({"colo":"LHR","asn":13335})");
    onset.setSpanId(0x00000000000000cd);
    setAttributes(onset.initAttributes(9));
  });
  return session;
}

// Starts a session whose onset has no optional fields, and whose info `setInfo` fills in.
void reportBareOnset(
    TestFixture& fixture, kj::FunctionParam<void(rpc::Trace::Onset::Builder)> setInfo) {
  TailSession session(fixture);
  auto stop = session.report(1, [&](EventsBuilder events) {
    auto event = events[0];
    // A span context with no span ID or trace flags, and IDs whose high half is zero.
    auto context = event.initSpanContext();
    context.initTraceId().setLow(0x2a);
    context.getInfo().setEmpty();
    event.initInvocationId().setLow(0x2b);
    event.setTimestampNs(-1'500'000);
    event.setSequence(0);
    auto onset = event.getEvent().initOnset();
    setInfo(onset);
  });
  KJ_EXPECT(stop);
  KJ_EXPECT(session.finish() == EventOutcome::OK);
}

// Fills `events` with one event of each kind after the onset, ending with the outcome.
constexpr uint EVENTS_AFTER_ONSET = 13;
void setEventsAfterOnset(EventsBuilder events) {
  uint i = 0;
  auto next = [&]() {
    auto event = events[i];
    setCommon(event, i + 1);
    ++i;
    return event.getEvent();
  };

  {
    auto open = next().initSpanOpen();
    open.setOperationName("fetch-span");
    open.setSpanId(0xffffffffffffffff);
    setFetchInfo(open.getInfo().initFetch(), "");
  }
  {
    auto update = next().initSpanUpdate();
    auto status = update.getInfo().initStatus();
    status.setCode(rpc::SpanStatusCode::ERROR);
    status.getMessage().setText("it failed");
  }
  {
    auto links = next().initSpanLinks().initLinks(2);
    auto context = links[0].initContext();
    context.initTraceId().setLow(0x1);
    context.getInfo().setSpanId(0x2);
    context.initTraceFlags().getValue().setSet(3);
    setAttribute(links[0].initAttributes(1)[0], "k", [](auto v) { v[0].setString("v"); }, 1);
    links[0].setDroppedAttributesCount(4);
    links[1].initContext().initTraceId().setHigh(0x5);
  }
  setAttributes(next().initAttribute(9));
  {
    auto log = next().initLog();
    log.setTimestampNs(1);
    log.setLogLevel(rpc::Trace::Log::Level::WARN);
    log.setMessage(R"(["hello",{"a":1}])");
    auto slots = log.initErrorInfo(3);
    auto withStack = slots[0].initInfo();
    withStack.setName("TypeError");
    withStack.setMessage("bad");
    withStack.setStack("    at f (a.js:1:1)");
    auto noStack = slots[1].initInfo();
    noStack.setName("Error");
    noStack.setMessage("no stack");
    slots[2].setNone();
  }
  {
    auto log = next().initLog();
    log.setLogLevel(rpc::Trace::Log::Level::DEBUG_);
    log.setMessage(R"(["cut off)");
    log.setTruncated(true);
  }
  {
    auto exception = next().initException();
    exception.setName("RangeError");
    exception.setMessage("out of range");
    exception.setStack("RangeError: out of range\n    at g (b.js:2:2)");
    exception.getCode().setText("ERR_OUT_OF_RANGE");
  }
  {
    auto exception = next().initException();
    exception.setName("Error");
    exception.setMessage("numbered");
    exception.getCode().setNumber(42.5);
  }
  {
    auto diagnostics = next().initStreamDiagnostics();
    diagnostics.getDiagnostic().initDroppedEvents().setCount(7);
  }
  next().initReturn().getInfo().initFetch().setStatusCode(204);
  next().initReturn().getInfo().setEmpty();
  next().initSpanClose().setOutcome(EventOutcome::EXCEEDED_CPU);
  {
    auto outcome = next().initOutcome();
    outcome.setOutcome(EventOutcome::OK);
    outcome.setCpuTime(12);
    outcome.setWallTime(345);
  }
  KJ_ASSERT(i == EVENTS_AFTER_ONSET);
}

KJ_TEST("a function handler receives every kind of event") {
  TestFixture fixture(tailWorker());
  auto session = startFull(fixture, "function");
  KJ_EXPECT(session->report(EVENTS_AFTER_ONSET, setEventsAfterOnset));
  KJ_EXPECT(session->finish() == EventOutcome::OK);
  expectGolden("function-handler", takeReceived(fixture));
}

KJ_TEST("an object handler is asked for the handler of each kind of event") {
  TestFixture fixture(tailWorker());
  auto session = startFull(fixture, "object");
  KJ_EXPECT(session->report(EVENTS_AFTER_ONSET, setEventsAfterOnset));
  KJ_EXPECT(session->finish() == EventOutcome::OK);
  expectGolden("object-handler", takeReceived(fixture));
}

KJ_TEST("setters on Object.prototype see the properties of each event set in order") {
  auto source = kj::str(SETTERS, TAIL_WORKER);
  TestFixture fixture({.mainModuleSource = source.asPtr()});
  auto session = startFull(fixture, "function");
  KJ_EXPECT(session->report(EVENTS_AFTER_ONSET, setEventsAfterOnset));
  KJ_EXPECT(session->finish() == EventOutcome::OK);
  expectGolden("prototype-setters", takeReceived(fixture));
}

KJ_TEST("spans are opened with each kind of info, renamed, and given each status") {
  TestFixture fixture(tailWorker());
  auto session = startFull(fixture, "function");
  session->report(9, [](EventsBuilder events) {
    for (auto i: kj::indices(events)) setCommon(events[i], i + 1);
    events[0].getEvent().initSpanOpen().getInfo().setEmpty();
    {
      auto open = events[1].getEvent().initSpanOpen();
      open.setOperationName("rpc");
      open.getInfo().initJsRpc().setMethodName("myMethod");
    }
    {
      auto open = events[2].getEvent().initSpanOpen();
      open.setOperationName("custom");
      setAttributes(open.getInfo().initCustom(9));
    }
    events[3].getEvent().initSpanUpdate().getInfo().setName("renamed");
    {
      auto status = events[4].getEvent().initSpanUpdate().getInfo().initStatus();
      status.setCode(rpc::SpanStatusCode::UNSET);
    }
    {
      // A message is kept only for an error.
      auto status = events[5].getEvent().initSpanUpdate().getInfo().initStatus();
      status.setCode(rpc::SpanStatusCode::OK);
      status.getMessage().setText("dropped");
    }
    {
      auto status = events[6].getEvent().initSpanUpdate().getInfo().initStatus();
      status.setCode(rpc::SpanStatusCode::ERROR);
      status.getMessage().setNone();
    }
    events[7].getEvent().initSpanClose().setOutcome(EventOutcome::ABORTED);
    events[8].getEvent().initAttribute(0);
  });
  session->report(1, [](EventsBuilder events) {
    setCommon(events[0], 10);
    events[0].getEvent().initOutcome().setOutcome(EventOutcome::EXCEPTION);
  });
  KJ_EXPECT(session->finish() == EventOutcome::OK);
  expectGolden("spans", takeReceived(fixture));
}

KJ_TEST("each outcome and log level has its name") {
  TestFixture fixture(tailWorker());
  auto session = startFull(fixture, "function");
  constexpr EventOutcome OUTCOMES[] = {
    EventOutcome::UNKNOWN,
    EventOutcome::OK,
    EventOutcome::EXCEPTION,
    EventOutcome::EXCEEDED_CPU,
    EventOutcome::KILL_SWITCH,
    EventOutcome::DAEMON_DOWN,
    EventOutcome::SCRIPT_NOT_FOUND,
    EventOutcome::CANCELED,
    EventOutcome::EXCEEDED_MEMORY,
    EventOutcome::LOAD_SHED,
    EventOutcome::RESPONSE_STREAM_DISCONNECTED,
    EventOutcome::INTERNAL_ERROR,
    EventOutcome::EXCEEDED_WALL_TIME,
    EventOutcome::ABORTED,
  };
  constexpr rpc::Trace::Log::Level LEVELS[] = {
    rpc::Trace::Log::Level::DEBUG_,
    rpc::Trace::Log::Level::INFO,
    rpc::Trace::Log::Level::LOG,
    rpc::Trace::Log::Level::WARN,
    rpc::Trace::Log::Level::ERROR,
  };
  session->report(kj::size(OUTCOMES) + kj::size(LEVELS), [&](EventsBuilder events) {
    uint i = 0;
    for (auto outcome: OUTCOMES) {
      setCommon(events[i], i + 1);
      events[i++].getEvent().initSpanClose().setOutcome(outcome);
    }
    for (auto level: LEVELS) {
      setCommon(events[i], i + 1);
      auto log = events[i++].getEvent().initLog();
      log.setLogLevel(level);
      log.setMessage("null");
    }
  });
  session->report(1, [&](EventsBuilder events) {
    setCommon(events[0], kj::size(OUTCOMES) + kj::size(LEVELS) + 1);
    events[0].getEvent().initOutcome().setOutcome(EventOutcome::CANCELED);
  });
  KJ_EXPECT(session->finish() == EventOutcome::OK);
  expectGolden("outcomes-and-levels", takeReceived(fixture));
}

KJ_TEST("a diagnostics channel message is deserialized") {
  TestFixture fixture(tailWorker());
  kj::Array<kj::byte> message;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;
    // Values of each kind that jsg::Serializer writes: plain data, collections, native errors,
    // a DOMException and a workerd API object.
    auto source = js.str(R"(({
      n: 1, s: "x", a: [true, null, undefined], big: 123n, date: new Date(5), re: /a+/gi,
      nested: { deep: [{ x: -0 }] },
      map: new Map([[1, "one"], ["k", { v: 2 }]]), set: new Set([2, "b"]),
      bytes: new Uint8Array([1, 2, 255]),
      error: Object.assign(new TypeError("boom"), { extra: "kept?" }),
      dom: new DOMException("aborted", "AbortError"),
      headers: new Headers([["a", "1"], ["b", "2"]]),
    }))"_kj);
    auto script = jsg::check(v8::Script::Compile(js.v8Context(), v8::Local<v8::String>(source)));
    auto object = jsg::JsValue(jsg::check(script->Run(js.v8Context())));
    jsg::Serializer serializer(js);
    serializer.write(js, object);
    message = serializer.release().data;
  });

  auto session = startFull(fixture, "function");
  session->report(2, [&](EventsBuilder events) {
    setCommon(events[0], 1);
    auto event = events[0].getEvent().initDiagnosticChannelEvent();
    event.setTimestampNs(9);
    event.setChannel("my-channel");
    event.setMessage(message);
    setCommon(events[1], 2);
    events[1].getEvent().initOutcome().setOutcome(EventOutcome::OK);
  });
  KJ_EXPECT(session->finish() == EventOutcome::OK);
  expectGolden("diagnostics-channel", takeReceived(fixture));
}

KJ_TEST("an onset has each kind of event info") {
  TestFixture fixture(tailWorker());
  reportBareOnset(fixture, [](auto onset) { onset.initInfo().initJsRpc().setMethodName("m"); });
  reportBareOnset(fixture, [](auto onset) {
    auto scheduled = onset.initInfo().initScheduled();
    scheduled.setScheduledTime(1234.5);
    scheduled.setCron("*/5 * * * *");
  });
  reportBareOnset(fixture,
      [](auto onset) { onset.initInfo().initAlarm().setScheduledTimeMs(1'700'000'000'000); });
  reportBareOnset(fixture, [](auto onset) {
    auto queue = onset.initInfo().initQueue();
    queue.setQueueName("q");
    queue.setBatchSize(10);
  });
  reportBareOnset(fixture, [](auto onset) {
    auto email = onset.initInfo().initEmail();
    email.setMailFrom("a@example.com");
    email.setRcptTo("b@example.com");
    email.setRawSize(99);
  });
  reportBareOnset(fixture, [](auto onset) {
    // A trace item without a script name.
    auto traces = onset.initInfo().initTrace().initTraces(2);
    traces[0].setScriptName("traced");
  });
  reportBareOnset(fixture,
      [](auto onset) { onset.initInfo().initHibernatableWebSocket().getType().setMessage(); });
  reportBareOnset(fixture, [](auto onset) {
    auto close = onset.initInfo().initHibernatableWebSocket().getType().initClose();
    close.setCode(1006);
    close.setWasClean(false);
  });
  reportBareOnset(fixture,
      [](auto onset) { onset.initInfo().initHibernatableWebSocket().getType().setError(); });
  reportBareOnset(fixture, [](auto onset) { onset.initInfo().initConnect(); });
  reportBareOnset(fixture, [](auto onset) {
    onset.initInfo().initOnHibernate().setReason(
        rpc::Trace::OnHibernateEventInfo::Reason::INACTIVE);
  });
  reportBareOnset(fixture, [](auto onset) {
    onset.setExecutionModel(rpc::Trace::ExecutionModel::WORKFLOW);
    onset.initInfo().initOnHibernate().setReason(
        rpc::Trace::OnHibernateEventInfo::Reason::CODE_UPDATED);
  });
  reportBareOnset(fixture, [](auto onset) {
    onset.initInfo().initOnHibernate().setReason(rpc::Trace::OnHibernateEventInfo::Reason::SYSTEM);
  });
  reportBareOnset(fixture, [](auto onset) { onset.initInfo().initCustom(); });
  expectGolden("onset-infos", takeReceived(fixture));
}

KJ_TEST("an enumerant unknown to the schema is named by its number") {
  TestFixture fixture(tailWorker());
  reportBareOnset(fixture, [](auto onset) {
    onset.setExecutionModel(static_cast<rpc::Trace::ExecutionModel>(7));
    onset.initInfo().initOnHibernate().setReason(
        static_cast<rpc::Trace::OnHibernateEventInfo::Reason>(9));
  });
  expectGolden("unknown-enumerants", takeReceived(fixture));
}

// Sends a batch whose second event `setBad` makes invalid, and checks that the session fails. The
// golden section `name` shows which events reached the handler first. If `log` is given, the
// failure is logged as an error that contains it.
void expectBatchFails(kj::StringPtr name,
    kj::Maybe<kj::StringPtr> log,
    kj::FunctionParam<void(rpc::Trace::TailEvent::Builder)> setBad) {
  TestFixture fixture(tailWorker());
  auto session = startFull(fixture, "function");
  auto report = [&]() {
    KJ_EXPECT(session->report(3, [&](EventsBuilder events) {
      for (auto i: kj::indices(events)) setCommon(events[i], i + 1);
      events[0].getEvent().initSpanUpdate().getInfo().setName("before");
      setBad(events[1]);
      events[2].getEvent().initSpanUpdate().getInfo().setName("after");
    }));
    KJ_EXPECT(session->finish() == EventOutcome::EXCEPTION);
  };
  KJ_IF_SOME(substring, log) {
    KJ_EXPECT_LOG(ERROR, substring);
    report();
  } else {
    report();
  }
  expectGolden(name, takeReceived(fixture));
}

KJ_TEST("a log message that is not JSON fails the session") {
  expectBatchFails(
      "bad-json", kj::none, [](auto event) { event.getEvent().initLog().setMessage("{not json"); });
}

KJ_TEST("an unknown HTTP method fails the session") {
  expectBatchFails("bad-method", "unknown method"_kj, [](auto event) {
    auto open = event.getEvent().initSpanOpen();
    open.getInfo().initFetch().setMethod(static_cast<capnp::HttpMethod>(99));
  });
}

KJ_TEST("an unknown event outcome fails the session") {
  expectBatchFails("bad-outcome", "Supposedly-unreachable branch executed"_kj, [](auto event) {
    event.getEvent().initSpanClose().setOutcome(static_cast<EventOutcome>(99));
  });
}

KJ_TEST("a stream diagnostics event without a diagnostic fails the session") {
  expectBatchFails("bad-diagnostics", "received invalid diagnostics event"_kj,
      [](auto event) { event.getEvent().initStreamDiagnostics(); });
}

KJ_TEST("text that is not NUL-terminated fails the session") {
  expectBatchFails(
      "bad-text", "Message contains text that is not NUL-terminated"_kj, [](auto event) {
    auto log = event.getEvent().initLog();
    log.setMessage("\"x\"");
    // A Data blob has no NUL terminator to read the text up to.
    capnp::AnyStruct::Builder(kj::mv(log)).getPointerSection()[0].setAs<capnp::Data>("\"x\""_kjb);
  });
}

KJ_TEST("a struct where text is expected fails the session") {
  expectBatchFails("bad-pointer", "Schema mismatch"_kj, [](auto event) {
    auto log = event.getEvent().initLog();
    capnp::AnyStruct::Builder(kj::mv(log)).getPointerSection()[0].initAsAnyStruct(1, 0);
  });
}

KJ_TEST("diagnostics channel data without a header fails the session") {
  expectBatchFails("bad-diagnostics-channel-header", kj::none, [](auto event) {
    auto dce = event.getEvent().initDiagnosticChannelEvent();
    dce.setChannel("channel");
    dce.setMessage("\x22\x01\x61"_kjb);
  });
}

}  // namespace
}  // namespace workerd::tracing
