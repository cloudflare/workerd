// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/io/observer.h>
#include <workerd/io/worker-interface.h>
#include <workerd/tests/test-fixture.h>
#include <workerd/util/stream-utils.h>

#include <kj/compat/http.h>
#include <kj/test.h>

namespace workerd {
namespace {

struct Events {
  kj::Vector<kj::String> lines;

  // Observers not yet destroyed, and the fulfiller to call when the last one goes. Issues are
  // written only then, which can be well after the response when waitUntil work is running.
  uint observers = 0;
  kj::Maybe<kj::Own<kj::PromiseFulfiller<void>>> allEnded;

  kj::String joined() const {
    return kj::strArray(lines, "\n");
  }
};

kj::String describe(issues::Issue::Field::Reader field) {
  auto value = field.getValue();
  switch (value.which()) {
    case issues::Issue::Field::Value::TEXT:
      return kj::str(field.getName(), "=", value.getText());
    case issues::Issue::Field::Value::INT:
      return kj::str(field.getName(), "=", value.getInt());
    case issues::Issue::Field::Value::FLOAT:
      return kj::str(field.getName(), "=", value.getFloat());
    case issues::Issue::Field::Value::FLAG:
      return kj::str(field.getName(), "=", value.getFlag());
  }
  KJ_UNREACHABLE;
}

kj::String describe(issues::Issue::Breadcrumb::Reader crumb) {
  if (crumb.getKind() == issues::Issue::Breadcrumb::Kind::LOG) {
    return kj::str("log ", crumb.getLevel(), " ", crumb.getLabel());
  }
  return kj::str("call ", crumb.getLabel());
}

// One line per issue, with the parts that are set separated by " | ".
kj::String describe(issues::Issue::Reader issue) {
  kj::Vector<kj::String> parts;
  parts.add(kj::str("issue ", issue.getCode(), " message=", issue.getMessage()));
  for (auto field: issue.getFields()) parts.add(describe(field));
  for (auto error: issue.getErrors()) {
    parts.add(kj::str("error ", error.getName(), ": ", error.getMessage(),
        error.getStack().size() == 0 ? " (no stack)" : " (stack)"));
  }
  for (auto attachment: issue.getAttachments()) {
    parts.add(kj::str(attachment.getName(), "=", attachment.getBody()));
  }
  if (issue.getBreadcrumbs().size() > 0) {
    kj::Vector<kj::String> crumbs;
    for (auto crumb: issue.getBreadcrumbs()) crumbs.add(describe(crumb));
    parts.add(kj::str("crumbs [", kj::strArray(crumbs, ", "), "]"));
  }
  auto user = issue.getUser();
  if (user.getUserId().size() > 0) parts.add(kj::str("user.id=", user.getUserId()));
  if (user.getSessionId().size() > 0) parts.add(kj::str("session.id=", user.getSessionId()));
  if (user.getAccountId().size() > 0) parts.add(kj::str("account.id=", user.getAccountId()));
  auto invocation = issue.getInvocation();
  parts.add(kj::str("invocation=", invocation.getEvent(), " ", invocation.getMethod(), " ",
      invocation.getPath()));
  parts.add(kj::str("status=", issue.getStatus()));
  parts.add(kj::str("outcome=",
      kj::StringPtr(capnp::Schema::from<EventOutcome>()
                        .getEnumerants()[static_cast<uint>(issue.getOutcome())]
                        .getProto()
                        .getName())));
  return kj::strArray(parts, " | ");
}

class RecordingSink final: public issues::IssueSink {
 public:
  explicit RecordingSink(Events& events): events(events) {}

  void write(issues::Issue::Reader issue) override {
    events.lines.add(describe(issue));
  }

 private:
  Events& events;
};

// Like an embedder's observer, it owns the collector and ends it when the last reference to it
// goes, with the outcome it saw.
class IssueObserver final: public RequestObserver {
 public:
  IssueObserver(Events& events, bool enabled): events(events) {
    ++events.observers;
    if (enabled) {
      auto& recorded = sink.emplace(events);
      collector.emplace(recorded);
    }
  }

  ~IssueObserver() noexcept(false) {
    KJ_IF_SOME(c, collector) {
      c.end(failed ? EventOutcome::EXCEPTION : EventOutcome::OK);
    }
    if (--events.observers == 0) {
      KJ_IF_SOME(f, events.allEnded) {
        f->fulfill();
      }
    }
  }

  void reportFailure(const kj::Exception& e, FailureSource source) override {
    failed = true;
  }

  kj::Maybe<issues::IssueCollector&> getIssueCollector() override {
    return collector;
  }

 private:
  Events& events;
  kj::Maybe<RecordingSink> sink;
  kj::Maybe<issues::IssueCollector> collector;
  bool failed = false;
};

TestFixture::SetupParams paramsFor(
    Events& events, bool enabled, kj::StringPtr source, bool useRealTimers = false) {
  return {
    .mainModuleSource = source,
    .useRealTimers = useRealTimers,
    .requestObserverFactory =
        kj::Function<kj::Own<RequestObserver>()>([&events, enabled]() -> kj::Own<RequestObserver> {
    return kj::refcounted<IssueObserver>(events, enabled);
  }),
  };
}

struct DiscardingStream final: kj::AsyncOutputStream {
  kj::Promise<void> write(kj::ArrayPtr<const byte>) override {
    return kj::READY_NOW;
  }
  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const byte>>) override {
    return kj::READY_NOW;
  }
  kj::Promise<void> whenWriteDisconnected() override {
    return kj::NEVER_DONE;
  }
};

struct StatusResponse final: kj::HttpService::Response {
  uint status = 0;

  kj::Own<kj::AsyncOutputStream> send(uint statusCode,
      kj::StringPtr,
      const kj::HttpHeaders&,
      kj::Maybe<uint64_t> = kj::none) override {
    status = statusCode;
    return kj::heap<DiscardingStream>();
  }

  kj::Own<kj::WebSocket> acceptWebSocket(const kj::HttpHeaders&) override {
    KJ_FAIL_REQUIRE("NOT SUPPORTED");
  }
};

// Sends one fetch through the production WorkerEntrypoint wrapper and returns the status, once
// every observer is gone and so every issue has been written.
uint fetchThroughEntrypoint(
    TestFixture& fixture, Events& events, kj::StringPtr url = "http://example.com/"_kj) {
  auto ended = kj::newPromiseAndFulfiller<void>();
  events.allEnded = kj::mv(ended.fulfiller);
  kj::HttpHeaderTable table;
  kj::HttpHeaders headers(table);
  StatusResponse response;
  auto body = newNullInputStream();
  {
    auto entrypoint = fixture.makeWorkerEntrypoint();
    entrypoint->request(kj::HttpMethod::GET, url, headers, *body, response)
        .wait(fixture.getWaitScope());
  }
  if (events.observers > 0) ended.promise.wait(fixture.getWaitScope());
  events.allEnded = kj::none;
  return response.status;
}

KJ_TEST("an observer with no sink gets no collector and builds nothing") {
  Events events;
  TestFixture fixture(paramsFor(events, false, R"JS(
    export default {
      async fetch() {
        console.error(new Error("boom"));
        return new Response("down", { status: 503 });
      }
    };
  )JS"_kj));

  KJ_EXPECT(fetchThroughEntrypoint(fixture, events) == 503);
  KJ_EXPECT(events.joined() == "", events.joined());
}

KJ_TEST("console.log leaves a breadcrumb and console.error reports an error_log") {
  Events events;
  TestFixture fixture(paramsFor(events, true, R"JS(
    export default {
      async fetch() {
        console.log("pricing sku-991");
        console.error(new TypeError("boom"), "while pricing");
        return new Response("ok");
      }
    };
  )JS"_kj));

  KJ_EXPECT(fetchThroughEntrypoint(fixture, events) == 200);
  KJ_EXPECT(events.joined() ==
          "issue error_log message=boom | level=error | error TypeError: boom (stack) | "
          "args=[\"TypeError: boom\", \"while pricing\"] | crumbs [log log pricing sku-991] | "
          "invocation=fetch GET / | status=200 | outcome=ok",
      events.joined());
}

KJ_TEST("error_log carries the first string argument as its message and drops extra errors") {
  Events events;
  TestFixture fixture(paramsFor(events, true, R"JS(
    export default {
      async fetch() {
        console.error("failed", 1, 2);
        console.warn("x".repeat(300));
        const errors = [1, 2, 3, 4, 5, 6].map((n) => new Error("e" + n));
        console.info("many", ...errors);
        return new Response("ok");
      }
    };
  )JS"_kj));

  KJ_EXPECT(fetchThroughEntrypoint(fixture, events) == 200);
  auto text = events.joined();
  KJ_EXPECT(text.startsWith("issue error_log message=failed | level=error | "
                            "args=[\"failed\", 1, 2] | invocation=fetch GET / | status=200"),
      text);
  KJ_EXPECT(text.contains("issue error_log message=many | level=info | "
                          "error Error: e1 (stack) | error Error: e2 (stack) | "
                          "error Error: e3 (stack) | error Error: e4 (stack) | args="),
      text);
  KJ_EXPECT(!text.contains("e5 (stack)"), text);
  KJ_EXPECT(text.contains("crumbs [log error failed, log warn xxxxxxxx"), text);
}

KJ_TEST("an uncaught handler exception reports uncaught_exception and drops the 500") {
  Events events;
  TestFixture fixture(paramsFor(events, true, R"JS(
    export default {
      async fetch() {
        throw new RangeError("out of range");
      }
    };
  )JS"_kj));

  KJ_EXPECT(fetchThroughEntrypoint(fixture, events) == 500);
  KJ_EXPECT(events.joined() ==
          "issue uncaught_exception message=out of range | "
          "error RangeError: out of range (stack) | invocation=fetch GET / | status=500 | "
          "outcome=exception",
      events.joined());
}

KJ_TEST("an error log before a 500 drops the 500 and one after does not") {
  Events events;
  TestFixture fixture(paramsFor(events, true, R"JS(
    export default {
      async fetch(request) {
        console.error("logged first");
        return new Response("down", { status: 500 });
      }
    };
  )JS"_kj));

  KJ_EXPECT(fetchThroughEntrypoint(fixture, events) == 500);
  KJ_EXPECT(events.joined() ==
          "issue error_log message=logged first | level=error | args=[\"logged first\"] | "
          "invocation=fetch GET / | status=500 | outcome=ok",
      events.joined());
}

KJ_TEST("a bare 503 reports http_server_error without an exception") {
  Events events;
  TestFixture fixture(paramsFor(events, true, R"JS(
    export default {
      async fetch() {
        return new Response("busy", { status: 503 });
      }
    };
  )JS"_kj));

  KJ_EXPECT(fetchThroughEntrypoint(fixture, events) == 503);
  KJ_EXPECT(events.joined() ==
          "issue http_server_error message=Worker returned HTTP 503 | status=503 | "
          "invocation=fetch GET / | status=503 | outcome=ok",
      events.joined());
}

KJ_TEST("an error log written in waitUntil after the response still produces its issue") {
  Events events;
  TestFixture fixture(paramsFor(events, true, R"JS(
    export default {
      async fetch(request, env, ctx) {
        ctx.waitUntil((async () => {
          await scheduler.wait(10);
          console.error("late");
        })());
        return new Response("ok");
      }
    };
  )JS"_kj,
      true));

  KJ_EXPECT(fetchThroughEntrypoint(fixture, events) == 200);
  KJ_EXPECT(events.joined() ==
          "issue error_log message=late | level=error | args=[\"late\"] | "
          "invocation=fetch GET / | status=200 | outcome=ok",
      events.joined());
}

KJ_TEST("the invocation records the path without its query") {
  Events events;
  TestFixture fixture(paramsFor(events, true, R"JS(
    export default {
      async fetch() {
        console.error("boom");
        return new Response("ok");
      }
    };
  )JS"_kj));

  KJ_EXPECT(fetchThroughEntrypoint(
                fixture, events, "http://example.com/checkout/cart?token=secret"_kj) == 200);
  KJ_EXPECT(events.joined().contains("invocation=fetch GET /checkout/cart |"), events.joined());
  KJ_EXPECT(!events.joined().contains("secret"), events.joined());
}

KJ_TEST("user.id, session.id and account.id span attributes reach the issue, and user spans "
        "leave no breadcrumb") {
  Events events;
  TestFixture fixture(paramsFor(events, true, R"JS(
    export default {
      async fetch(request, env, ctx) {
        ctx.tracing.enterSpan("checkout", (span) => {
          span.setAttribute("user.id", "u_4821");
          span.setAttribute("session.id", "s_9");
          span.setAttribute("account.id", "a_1");
          span.setAttribute("cart.size", "3");
          span.setAttribute("user.id", 7);
        });
        console.error("after");
        return new Response("ok");
      }
    };
  )JS"_kj));

  KJ_EXPECT(fetchThroughEntrypoint(fixture, events) == 200);
  KJ_EXPECT(events.joined() ==
          "issue error_log message=after | level=error | args=[\"after\"] | "
          "user.id=u_4821 | session.id=s_9 | account.id=a_1 | invocation=fetch GET / | "
          "status=200 | outcome=ok",
      events.joined());
}

}  // namespace
}  // namespace workerd
