// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/api/tracing.h>
#include <workerd/io/otlp-span-submitter.h>
#include <workerd/io/timer-test-fixture.h>
#include <workerd/io/trace-stream.h>
#include <workerd/io/tracer.h>
#include <workerd/tests/test-fixture.h>

#include <opentelemetry/proto/collector/logs/v1/logs_service.pb.h>

#include <kj/test.h>

namespace workerd {
namespace {

class RecordingSpanSubmitter final: public SpanSubmitter {
 public:
  kj::Vector<kj::Date> starts;
  kj::Vector<kj::Date> exceptions;

  bool submitSpanOpen(tracing::SpanId spanId,
      tracing::SpanId parentSpanId,
      kj::ConstString operationName,
      kj::Date startTime) override {
    starts.add(startTime);
    return true;
  }

  void submitSpanClose(
      tracing::SpanId spanId, kj::Date startTime, kj::Date endTime, Span::TagMap&& tags) override {}
  void submitSpanUpdate(tracing::SpanId spanId, tracing::SpanUpdate&& update) override {}
  void submitSpanException(tracing::SpanId spanId,
      kj::Date timestamp,
      kj::Maybe<tracing::Exception::Code> code,
      kj::String name,
      kj::String message,
      kj::Maybe<kj::String> stack) override {
    exceptions.add(timestamp);
  }
  tracing::SpanId makeSpanId() override {
    return tracing::SpanId(nextId++);
  }

 private:
  uint64_t nextId = 1;
};

class TestEntropySource final: public kj::EntropySource {
 public:
  void generate(kj::ArrayPtr<kj::byte> buffer) override {
    for (auto& byte: buffer) byte = next++;
  }

 private:
  kj::byte next = 1;
};

namespace otlp = ::opentelemetry::proto;

class RecordingExporter final: public OtlpSpanExporter {
 public:
  uint spanCount = 0;
  uint logCount = 0;
  kj::Vector<otlp::collector::logs::v1::ExportLogsServiceRequest> logRequests;

  void exportSpans(kj::Array<const kj::byte> request, uint count) override {
    spanCount += count;
  }
  void exportLogs(kj::Array<const kj::byte> request, uint count) override {
    logCount += count;
    KJ_ASSERT(logRequests.add().ParseFromArray(request.begin(), request.size()));
  }
};

// The records of a request that holds one resource's logs.
const google::protobuf::RepeatedPtrField<otlp::logs::v1::LogRecord>& records(
    const otlp::collector::logs::v1::ExportLogsServiceRequest& request) {
  KJ_ASSERT(request.resource_logs_size() == 1);
  KJ_ASSERT(request.resource_logs(0).scope_logs_size() == 1);
  return request.resource_logs(0).scope_logs(0).log_records();
}

const otlp::common::v1::AnyValue& attribute(
    const otlp::logs::v1::LogRecord& record, kj::StringPtr key) {
  for (auto& kv: record.attributes()) {
    if (kv.key() == key.cStr()) return kv.value();
  }
  KJ_FAIL_ASSERT("missing attribute", key);
}

void expectPreciseTime(SpanObserver& observer) {
  auto before = kj::systemPreciseCalendarClock().now();
  auto time = observer.getTime();
  auto after = kj::systemPreciseCalendarClock().now();
  KJ_EXPECT(time >= before && time <= after);
}

void expectNativeClocks() {
  TestEntropySource entropy;
  auto exporter = kj::rc<RecordingExporter>();
  {
    auto root = startExportedUserSpan(entropy, exporter.addRef(),
        tracing::SpanContext(tracing::TraceId(1, 2), tracing::SpanId(3)),
        {.serviceName = "clock-test"}, "root"_kjc);
    expectPreciseTime(KJ_ASSERT_NONNULL(root.getObserver()));
    auto child = root.newChild("runtime"_kjc);
    expectPreciseTime(KJ_ASSERT_NONNULL(child.getObserver()));
    auto user =
        SpanBuilder(KJ_ASSERT_NONNULL(child.getObserver()).newChildFromUserCode(), "user"_kjc);
    expectPreciseTime(KJ_ASSERT_NONNULL(user.getObserver()));
    auto nested = user.newChild("nested"_kjc);
    expectPreciseTime(KJ_ASSERT_NONNULL(nested.getObserver()));
    user.recordException(kj::none, kj::str("Error"), kj::str("test"), kj::none);
  }
  KJ_EXPECT(exporter->spanCount == 4);
}

KJ_TEST("streaming spans use I/O time while native spans use precise time") {
  test::TestTimerChannel timer(test::TimerClock::CLAMPED);
  timer.advanceTo(123);
  TestFixture fixture({.ioChannelFactory = kj::Function<kj::Rc<IoChannelFactory>(TimerChannel&)>(
                           [&](TimerChannel&) -> kj::Rc<IoChannelFactory> {
    return kj::rc<TestFixture::DummyIoChannelFactory>(timer);
  })});
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    KJ_ASSERT(!env.features.getPreciseTimers());
    auto time = kj::UNIX_EPOCH + 123 * kj::MILLISECONDS;
    KJ_ASSERT(env.context.now() == time);
    auto submitter = kj::refcounted<RecordingSpanSubmitter>();
    auto observer = kj::rc<UserSpanObserver>(kj::addRef(*submitter));
    KJ_EXPECT(observer->getTime() == time);
    auto runtime = SpanBuilder(observer->newChild(), "runtime"_kjc);
    auto user =
        SpanBuilder(KJ_ASSERT_NONNULL(runtime.getObserver()).newChildFromUserCode(), "user"_kjc);
    auto nested = user.newChild("nested"_kjc);
    user.recordException(kj::none, kj::str("Error"), kj::str("test"), kj::none);
    KJ_ASSERT(submitter->starts.size() == 3);
    for (auto start: submitter->starts) KJ_EXPECT(start == time);
    KJ_ASSERT(submitter->exceptions.size() == 1);
    KJ_EXPECT(submitter->exceptions[0] == time);

    expectNativeClocks();
  });
}

KJ_TEST("native span clocks work without an IoContext") {
  expectNativeClocks();
}

KJ_TEST("native invocation exceptions reach streaming tails with I/O timestamps") {
  test::TestTimerChannel timer(test::TimerClock::CLAMPED);
  timer.advanceTo(123);
  TestFixture fixture({.ioChannelFactory = kj::Function<kj::Rc<IoChannelFactory>(TimerChannel&)>(
                           [&](TimerChannel&) -> kj::Rc<IoChannelFactory> {
    return kj::rc<TestFixture::DummyIoChannelFactory>(timer);
  })});
  TestFixture tail({
    .waitScope = fixture.getWaitScope(),
    .mainModuleSource = R"JS(
      const timestamps = [];
      export default {
        tailStream() {
          return event => {
            if (event.event.type === 'exception') timestamps.push(event.timestamp.getTime());
          };
        },
        fetch() { return Response.json(timestamps); }
      };
    )JS"_kj,
  });
  struct TaskErrors final: public kj::TaskSet::ErrorHandler {
    void taskFailed(kj::Exception&& exception) override {
      KJ_FAIL_ASSERT(exception);
    }
  } errors;
  kj::TaskSet tasks(errors);
  TestEntropySource entropy;
  auto exporter = kj::rc<RecordingExporter>();
  auto trace = kj::refcounted<Trace>(kj::none, kj::none, kj::none, kj::none, kj::none,
      kj::Array<kj::String>(), kj::none, ExecutionModel::STATELESS);
  auto tracer = kj::rc<WorkerTracer>(kj::none, kj::mv(trace), PipelineLogLevel::FULL, kj::none,
      kj::heap<tracing::TailStreamWriter>(kj::arr(tail.makeWorkerEntrypoint()), tasks));
  tracer->setStartInvocationSpanFunc([&](tracing::InvocationSpanContext& context) {
    auto root = startExportedUserSpan(entropy, exporter.addRef(),
        tracing::SpanContext(context.getTraceId(), context.getSpanId(), context.getTraceFlags()),
        {.serviceName = "clock-test"}, "root"_kjc);
    expectPreciseTime(KJ_ASSERT_NONNULL(root.getObserver()));
    return root;
  });
  auto context = fixture.newIoContext();
  auto request = fixture.newUndeliveredIncomingRequest(*context, tracer.addRef());
  tracer->setEventInfo(*request, tracing::CustomEventInfo());
  request->delivered();
  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    KJ_ASSERT(env.context.now() == kj::UNIX_EPOCH + 123 * kj::MILLISECONDS);
    auto scope = env.context.makeRequestTraceScope(env.lock);
    auto tracing = env.js.alloc<api::Tracing>();
    auto& handler =
        KJ_ASSERT_NONNULL(env.js.tryGetTypeHandler<jsg::Ref<api::user_tracing::Span>>());
    auto span = KJ_ASSERT_NONNULL(tracing->getActiveSpan(env.js, handler));
    KJ_ASSERT(span->getIsTraced());
    auto object = jsg::JsObject(handler.wrap(env.js, kj::mv(span)).As<v8::Object>());
    auto record =
        KJ_ASSERT_NONNULL(object.get(env.js, "recordException"_kj).tryCast<jsg::JsFunction>());
    record.call(env.js, object, env.js.str("test exception"_kj));
  });
  tracer->setOutcome(EventOutcome::OK, 0 * kj::MILLISECONDS, 0 * kj::MILLISECONDS);
  tracer->recordTimestamp(kj::UNIX_EPOCH + 123 * kj::MILLISECONDS);
  fixture.drainAndDestroy(kj::mv(request));
  tracer = nullptr;
  tasks.onEmpty().wait(fixture.getWaitScope());
  auto response = tail.runRequest(kj::HttpMethod::GET, "https://tail/", "");
  KJ_EXPECT(response.statusCode == 200);
  KJ_EXPECT(response.body == "[123]");
  KJ_EXPECT(exporter->spanCount == 1);
}

// Runs an invocation that logs and throws while its span is open, and logs again after it closed.
kj::Rc<RecordingExporter> exportInvocation(ExportLogs exportLogs) {
  TestFixture fixture;
  TestEntropySource entropy;
  auto exporter = kj::rc<RecordingExporter>();
  auto trace = kj::refcounted<Trace>(kj::none, kj::none, kj::none, kj::none, kj::none,
      kj::Array<kj::String>(), kj::none, ExecutionModel::STATELESS);
  auto tracer =
      kj::rc<WorkerTracer>(kj::none, kj::mv(trace), PipelineLogLevel::FULL, kj::none, kj::none);
  tracer->setStartInvocationSpanFunc([&](tracing::InvocationSpanContext& context) {
    return startExportedInvocationSpan(entropy, exporter.addRef(),
        tracing::SpanContext(context.getTraceId(), kj::none, context.getTraceFlags()),
        {.serviceName = "log-test", .exportLogs = exportLogs}, context.getSpanId());
  });
  auto context = fixture.newIoContext();
  auto request = fixture.newUndeliveredIncomingRequest(*context, tracer.addRef());
  tracer->setEventInfo(*request, tracing::CustomEventInfo());
  request->delivered();
  kj::Maybe<tracing::InvocationSpanContext> spanContext;
  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    auto& c = spanContext.emplace(env.context.getInvocationSpanContext());
    // The second argument was an Error, which the JSON message does not describe.
    auto errorInfo = kj::heapArrayBuilder<kj::Maybe<tracing::ErrorInfo>>(2);
    errorInfo.add(kj::none);
    errorInfo.add(tracing::ErrorInfo(kj::str("TypeError"), kj::str("bad"), kj::str("at f")));
    tracer->addLog(
        c, env.context.now(), LogLevel::LOG, kj::str("[\"in-invocation\",{}]"), errorInfo.finish());
    tracer->addException(c, env.context.now(), kj::str("Error"), kj::str("boom"), kj::none);
  });
  // The invocation's logs wait for its span.
  KJ_EXPECT(exporter->logCount == 0);
  tracer->setOutcome(EventOutcome::OK, 0 * kj::MILLISECONDS, 0 * kj::MILLISECONDS);
  // Like the streaming tail, the export takes logs until the tracer goes away.
  tracer->addLog(KJ_ASSERT_NONNULL(spanContext), kj::UNIX_EPOCH, LogLevel::WARN,
      kj::str("[\"after-outcome\"]"));
  fixture.drainAndDestroy(kj::mv(request));
  tracer = nullptr;
  KJ_EXPECT(exporter->spanCount == 1);
  return exporter;
}

KJ_TEST("an exported invocation exports its logs and exceptions") {
  auto exporter = exportInvocation(ExportLogs::YES);
  KJ_ASSERT(exporter->logRequests.size() == 2);
  KJ_EXPECT(exporter->logCount == 3);
  auto& resource = exporter->logRequests[0].resource_logs(0).resource();
  KJ_EXPECT(resource.attributes(0).key() == "service.name");
  KJ_EXPECT(resource.attributes(0).value().string_value() == "log-test");

  auto& first = records(exporter->logRequests[0]);
  KJ_ASSERT(first.size() == 2);
  auto& log = first[0];
  KJ_EXPECT(log.severity_text() == "log");
  KJ_EXPECT(log.body().array_value().values(0).string_value() == "in-invocation");
  auto& arguments = attribute(log, "cloudflare.log.error_info").array_value();
  KJ_ASSERT(arguments.values_size() == 2);
  KJ_EXPECT(arguments.values(0).value_case() == otlp::common::v1::AnyValue::VALUE_NOT_SET);
  auto& error = arguments.values(1).kvlist_value();
  KJ_ASSERT(error.values_size() == 3);
  KJ_EXPECT(error.values(0).key() == "name");
  KJ_EXPECT(error.values(0).value().string_value() == "TypeError");
  KJ_EXPECT(error.values(1).key() == "message");
  KJ_EXPECT(error.values(1).value().string_value() == "bad");
  KJ_EXPECT(error.values(2).key() == "stack");
  KJ_EXPECT(error.values(2).value().string_value() == "at f");

  auto& exception = first[1];
  KJ_EXPECT(exception.event_name() == "exception");
  KJ_EXPECT(exception.body().string_value() == "boom");
  KJ_EXPECT(attribute(exception, "exception.type").string_value() == "Error");

  auto& late = records(exporter->logRequests[1]);
  KJ_ASSERT(late.size() == 1);
  KJ_EXPECT(late[0].severity_text() == "warn");
  KJ_EXPECT(late[0].body().array_value().values(0).string_value() == "after-outcome");
  // Without Error arguments a record has no error info.
  for (auto& kv: late[0].attributes()) KJ_EXPECT(kv.key() != "cloudflare.log.error_info");
}

KJ_TEST("an exported invocation exports no logs unless asked to") {
  auto exporter = exportInvocation(ExportLogs::NO);
  KJ_EXPECT(exporter->logCount == 0);
}

}  // namespace
}  // namespace workerd
