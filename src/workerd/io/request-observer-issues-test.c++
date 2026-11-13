// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/io/observer.h>
#include <workerd/io/worker-interface.h>
#include <workerd/tests/test-fixture.h>

#include <kj/compat/http.h>
#include <kj/test.h>

namespace workerd {
namespace {

struct Events {
  kj::Vector<kj::String> lines;

  kj::String joined() const {
    return kj::strArray(lines, "\n");
  }
};

kj::String describe(const issues::Field& field) {
  KJ_SWITCH_ONEOF(field.value) {
    KJ_CASE_ONEOF(text, kj::StringPtr) {
      return kj::str(field.name, "=", text);
    }
    KJ_CASE_ONEOF(number, int64_t) {
      return kj::str(field.name, "=", number);
    }
    KJ_CASE_ONEOF(number, double) {
      return kj::str(field.name, "=", number);
    }
    KJ_CASE_ONEOF(flag, bool) {
      return kj::str(field.name, "=", flag);
    }
  }
  KJ_UNREACHABLE;
}

class IssueRecorder final: public RequestObserver {
 public:
  IssueRecorder(Events& events, bool enabled): events(events), enabled(enabled) {}

  bool issuesEnabled() override {
    return enabled;
  }

  void reportIssue(kj::StringPtr code,
      kj::StringPtr message,
      kj::ArrayPtr<const issues::Field> fields,
      kj::ArrayPtr<const issues::Error> errors,
      kj::ArrayPtr<issues::Attachment> attachments) override {
    kj::Vector<kj::String> parts;
    parts.add(kj::str("issue ", code, " message=", message));
    for (auto& field: fields) parts.add(describe(field));
    for (auto& error: errors) {
      parts.add(kj::str("error ", error.name, ": ", error.message,
          error.stack == kj::none ? " (no stack)" : " (stack)"));
    }
    for (auto& attachment: attachments) {
      parts.add(kj::str(attachment.name, "=", attachment.render()));
    }
    events.lines.add(kj::strArray(parts, " | "));
  }

  void issueBreadcrumb(
      issues::BreadcrumbKind kind, kj::StringPtr detail, kj::StringPtr label) override {
    kj::StringPtr name = "call"_kj;
    switch (kind) {
      case issues::BreadcrumbKind::CALL_STARTED:
        name = "call started"_kj;
        break;
      case issues::BreadcrumbKind::CALL_FINISHED_OK:
        name = "call ok"_kj;
        break;
      case issues::BreadcrumbKind::CALL_FINISHED_FAILED:
        name = "call failed"_kj;
        break;
      case issues::BreadcrumbKind::LOG:
        name = "log"_kj;
        break;
    }
    events.lines.add(kj::str("breadcrumb ", name, " ", detail, " ", label));
  }

  void noteIssueUserContext(kj::StringPtr key, kj::StringPtr value) override {
    events.lines.add(kj::str("user ", key, "=", value));
  }

  void setIssueStatus(uint status) override {
    events.lines.add(kj::str("status ", status));
  }

 private:
  Events& events;
  bool enabled;
};

TestFixture::SetupParams paramsFor(Events& events, bool enabled, kj::StringPtr source) {
  return {
    .mainModuleSource = source,
    .requestObserverFactory =
        kj::Function<kj::Own<RequestObserver>()>([&events, enabled]() -> kj::Own<RequestObserver> {
    return kj::refcounted<IssueRecorder>(events, enabled);
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

// Sends one fetch through the production WorkerEntrypoint wrapper and returns the status.
uint fetchThroughEntrypoint(TestFixture& fixture) {
  kj::HttpHeaderTable table;
  kj::HttpHeaders headers(table);
  StatusResponse response;
  auto entrypoint = fixture.makeWorkerEntrypoint();
  entrypoint
      ->request(kj::HttpMethod::GET, "http://example.com/"_kj, headers,
          *kj::heap<kj::NullInputStream>(), response)
      .wait(fixture.getWaitScope());
  return response.status;
}

KJ_TEST("a disabled observer receives no issue hooks") {
  Events events;
  TestFixture fixture(paramsFor(events, false, R"JS(
    export default {
      async fetch() {
        console.error(new Error("boom"));
        return new Response("down", { status: 503 });
      }
    };
  )JS"_kj));

  KJ_EXPECT(fetchThroughEntrypoint(fixture) == 503);
  KJ_EXPECT(events.joined() == "", events.joined());
}

KJ_TEST("console.log only leaves a breadcrumb and console.error reports an error_log") {
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

  KJ_EXPECT(fetchThroughEntrypoint(fixture) == 200);
  KJ_EXPECT(events.joined() ==
          "breadcrumb log log pricing sku-991\n"
          "breadcrumb log error \n"
          "issue error_log message= | level=error | error TypeError: boom (stack) | "
          "args=[\"TypeError: boom\", \"while pricing\"]\n"
          "status 200",
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

  KJ_EXPECT(fetchThroughEntrypoint(fixture) == 200);
  auto text = events.joined();
  KJ_EXPECT(
      text.startsWith("breadcrumb log error failed\n"
                      "issue error_log message=failed | level=error | args=[\"failed\", 1, 2]\n"
                      "breadcrumb log warn xxxxxxxx"),
      text);
  KJ_EXPECT(text.contains("issue error_log message=many | level=info | "
                          "error Error: e1 (stack) | error Error: e2 (stack) | "
                          "error Error: e3 (stack) | error Error: e4 (stack) | args="),
      text);
  KJ_EXPECT(!text.contains("e5 (stack)"), text);
}

KJ_TEST("an uncaught handler exception reports uncaught_exception and then the 500") {
  Events events;
  TestFixture fixture(paramsFor(events, true, R"JS(
    export default {
      async fetch() {
        throw new RangeError("out of range");
      }
    };
  )JS"_kj));

  KJ_EXPECT(fetchThroughEntrypoint(fixture) == 500);
  KJ_EXPECT(events.joined() ==
          "issue uncaught_exception message=out of range | error RangeError: out of range (stack)\n"
          "status 500\n"
          "issue http_server_error message=Worker returned HTTP 500 | status=500",
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

  KJ_EXPECT(fetchThroughEntrypoint(fixture) == 503);
  KJ_EXPECT(events.joined() ==
          "status 503\n"
          "issue http_server_error message=Worker returned HTTP 503 | status=503",
      events.joined());
}

KJ_TEST("user.id, session.id and account.id span attributes reach the observer") {
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
        return new Response("ok");
      }
    };
  )JS"_kj));

  KJ_EXPECT(fetchThroughEntrypoint(fixture) == 200);
  KJ_EXPECT(events.joined() ==
          "breadcrumb call started  checkout\n"
          "user user.id=u_4821\n"
          "user session.id=s_9\n"
          "user account.id=a_1\n"
          "status 200",
      events.joined());
}

}  // namespace
}  // namespace workerd
