// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/api/pyodide-api/ffi.rs.h>
#include <workerd/io/io-context.h>
#include <workerd/io/observer.h>
#include <workerd/jsg/modules.h>
#include <workerd/rust/jsg/jsg.h>
#include <workerd/tests/test-fixture.h>

#include <kj/test.h>

namespace workerd::api::pyodide {
namespace {

struct Reports {
  uint fatal = 0;
  uint internalError = 0;
};

// Counts the reports that reach the metrics of a request.
class ReportCountingObserver final: public RequestObserver {
 public:
  explicit ReportCountingObserver(Reports& reports): reports(reports) {}

  void setWorkerFatal() override {
    ++reports.fatal;
  }
  void setPythonWorkersInternalError() override {
    ++reports.internalError;
  }

 private:
  Reports& reports;
};

// Calls the method `name` of `object` with `args`.
void call(jsg::Lock& js,
    v8::Local<v8::Object> object,
    kj::StringPtr name,
    kj::ArrayPtr<v8::Local<v8::Value>> args) {
  auto context = js.v8Context();
  auto method = jsg::check(object->Get(context, jsg::v8StrIntern(js.v8Isolate, name)));
  KJ_ASSERT(method->IsFunction(), name);
  jsg::check(method.As<v8::Function>()->Call(context, object, args.size(), args.begin()));
}

KJ_TEST("WorkerFatalReporter records its reports in the metrics of the current request") {
  // The type wrapper is used only to register C++ resources, which this test doesn't.
  struct UnusedTypeWrapper {};
  jsg::CompilationObserver compilationObserver;
  jsg::ModuleRegistryImpl<UnusedTypeWrapper> registry(compilationObserver);
  ::workerd::rust::jsg::RustModuleRegistry<decltype(registry), SuppressIoContextScope> adapter(
      registry);
  register_fatal_reporter(adapter);

  Reports reports;
  TestFixture fixture({
    .requestObserverFactory =
        kj::Function<kj::Own<RequestObserver>()>([&]() -> kj::Own<RequestObserver> {
    return kj::refcounted<ReportCountingObserver>(reports);
  }),
  });
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;
    auto& info =
        KJ_ASSERT_NONNULL(registry.resolve(js, kj::Path::parse("pyodide-internal:fatal-reporter"),
            kj::none, jsg::ModuleRegistry::ResolveOption::INTERNAL_ONLY));
    // The module's default export, which the registry made when it resolved the module.
    auto& synthetic = KJ_ASSERT_NONNULL(info.maybeSynthetic);
    auto reporter = synthetic.get<jsg::ModuleRegistry::ObjectModuleInfo>().value.getHandle(js);

    v8::Local<v8::Value> error = jsg::v8Str(js.v8Isolate, "RuntimeError: unreachable"_kj);
    call(js, reporter, "reportFatal"_kj, kj::arrayPtr(error));
    KJ_EXPECT(reports.fatal == 1, reports.fatal);
    KJ_EXPECT(reports.internalError == 0, reports.internalError);

    call(js, reporter, "reportPythonWorkersInternalError"_kj, nullptr);
    KJ_EXPECT(reports.fatal == 1, reports.fatal);
    KJ_EXPECT(reports.internalError == 1, reports.internalError);
  });
}

}  // namespace
}  // namespace workerd::api::pyodide
