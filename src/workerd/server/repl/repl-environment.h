// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/io/compatibility-date.capnp.h>
#include <workerd/io/io-context.h>
#include <workerd/io/io-thread-context.h>
#include <workerd/io/worker.h>
#include <workerd/jsg/script.h>
#include <workerd/server/repl/repl-api.h>
#include <workerd/server/repl/repl-server.h>

#include <capnp/compat/byte-stream.h>
#include <capnp/compat/http-over-capnp.h>
#include <capnp/message.h>
#include <kj/function.h>

namespace workerd::api {
class MemoryCacheProvider;
}  // namespace workerd::api

namespace workerd::server {

struct ReplEnvironmentOptions {
  // The compatibility date of the REPL's JavaScript; today if none. Like `workerd test
  // --compat-date`, any valid date is accepted, such as 2999-12-31 for every flag's behavior.
  kj::Maybe<kj::String> compatibilityDate;

  // Added to the date's flags. `nodejs_compat` is added too unless these take a position on it.
  kj::Array<kj::String> compatibilityFlags;

  // `--experimental`: lets the compatibility flags and the loaded configs use experimental
  // features, and adds `service_binding_extra_handlers` to the flags.
  bool experimental = false;

  // Render results with ANSI colors.
  bool colors = false;
};

// The JavaScript environment `workerd repl` evaluates input in: a worker that never receives an
// event, with one IoContext for the whole session, in which every input runs.
//
// The worker exists because the APIs need one: an IoContext belongs to a worker, and the global
// scope is a worker's. Its main module is empty and it has no bindings; instead its global scope
// has `workerd` (ReplControl), through which the user loads configs. It always uses the new module
// registry, whose built-in `node-internal:internal_inspect` renders results, and under which a
// dynamic `import()` from the REPL resolves as it would from the worker's main module.
//
// Lives on the thread of the event loop it was created on, and must be used only from there.
class ReplEnvironment final: public ReplHost {
 public:
  // One way to compile an input. The REPL tries an input's candidates in order and evaluates the
  // first that compiles; if none does, the first candidate's syntax error is the result. A
  // candidate that isn't awaited and uses `await` as a name, as `await (x)` does in a script, is
  // evaluated only if no later candidate compiles: an input that says `await` means to await.
  struct Candidate {
    kj::String source;
    // The completion value is a promise the REPL awaits, rendering what it settles to: the input
    // was wrapped in an async function so that it could `await`.
    bool awaited;
  };

  ReplEnvironment(const ReplServerResources& resources,
      ReplConfigLoader& configLoader,
      ReplEnvironmentOptions options);
  ~ReplEnvironment() noexcept(false);
  KJ_DISALLOW_COPY_AND_MOVE(ReplEnvironment);

  // Evaluates one input, resolving to the text to print: the rendered result, or "Uncaught "
  // followed by the rendered exception. A promise result is rendered as a promise unless the
  // candidate is `awaited`; the event loop runs while one is awaited. Rejects only if the
  // environment itself fails.
  kj::Promise<kj::String> evaluate(kj::Array<Candidate> candidates);

  // The string-named properties, own and inherited, of the value of `base` (an identifier chain
  // such as `a.b.c`), or of the global object if `base` is empty, for tab completion. Evaluating
  // `base` may run property getters, as completion in Node's REPL does. Empty if `base` is not an
  // identifier chain, its evaluation throws, or its value is null or undefined.
  kj::Promise<kj::Array<kj::String>> propertyNames(kj::String base);

  // Closes every loaded config's server, then ends the session's IoContext once its waitUntil()
  // tasks are done. Must complete before destruction; nothing else may be called afterwards.
  // Rejects with the first server's failure to close, if any, once all of that is done.
  kj::Promise<void> drain();

  kj::Promise<kj::Own<ReplServerHost>> loadConfig(ReplConfigSource source) override;

 private:
  class ChannelFactory;

  struct ErrorReporter final: public Worker::ValidationErrorReporter {
    kj::Vector<kj::String> errors;
    kj::Vector<kj::String> warnings;

    void addError(kj::String error) override {
      errors.add(kj::mv(error));
    }
    void addWarning(kj::String warning) override {
      warnings.add(kj::mv(warning));
    }
    void addEntrypoint(
        kj::Maybe<kj::StringPtr> exportName, kj::Array<kj::String> methods) override {}
    void addActorClass(kj::StringPtr exportName) override {}
    void addWorkflowClass(kj::StringPtr exportName, kj::Array<kj::String> methods) override {}

    // Throws, listing the errors, if there are any.
    void check(kj::StringPtr what);
  };

  ReplServerResources resources;
  ReplConfigLoader& configLoader;
  ReplEnvironmentOptions options;

  capnp::ByteStreamFactory byteStreamFactory;
  kj::HttpHeaderTable::Builder headerTableBuilder;
  ThreadContext::HeaderIdBundle threadContextHeaderBundle;
  capnp::HttpOverCapnpFactory httpOverCapnpFactory;
  ThreadContext threadContext;
  kj::Own<TimerChannel> timerChannel;

  // Owns the compatibility flags `featureFlags` reads.
  capnp::MallocMessageBuilder featureFlagsMessage;
  CompatibilityFlags::Reader featureFlags;

  kj::Own<api::MemoryCacheProvider> memoryCacheProvider;

  // The name REPL input is compiled under: the URL of the worker's main module. A dynamic import's
  // referrer determines where it resolves, so an `import()` typed into the REPL resolves as one in
  // the main module would.
  kj::String inputName;

  kj::Own<Worker::Isolate> isolate;
  kj::Own<const Worker::Script> script;
  kj::Own<const Worker> worker;

  // Sets `_` or `_error` (its first argument) to an input's outcome (its second); see
  // OUTCOMES_SCRIPT. None only while the worker is compiled.
  kj::Maybe<jsg::JsRef<jsg::JsFunction>> recordOutcome;

  kj::Own<kj::TaskSet::ErrorHandler> waitUntilErrorHandler;
  kj::TaskSet waitUntilTasks;
  kj::Own<kj::HttpHeaderTable> headerTable;

  // The session's request, which owns its IoContext. None once drained.
  kj::Maybe<kj::Own<IoContext::IncomingRequest>> incomingRequest;

  // The event the session's IoContext waits for between inputs: the next line of input. Without
  // it, a context with nothing pending is taken to be hung when the thread goes idle. Declared
  // after `incomingRequest` so that it is released while the context still exists.
  kj::Own<void> waitingForInput;

  // The servers of the configs loaded and not yet closed.
  kj::Vector<kj::Own<ReplConfigServer>> servers;

  void compileWorker();
  void startSession();

  struct Compiled {
    jsg::NonModuleScript script;
    bool awaited;
  };
  Compiled compile(jsg::Lock& js, kj::ArrayPtr<const Candidate> candidates);

  // An awaited result: the promise, and when it settles.
  struct Awaiting {
    jsg::JsRef<jsg::JsPromise> result;
    kj::Promise<void> settled;
  };
  // Runs the compiled input: the rendered result, or the awaited one.
  kj::OneOf<kj::String, Awaiting> run(
      jsg::Lock& js, IoContext& context, kj::ArrayPtr<const Candidate> candidates);

  // Records `value` as the last result (`name` is "_") or exception ("_error").
  void record(jsg::Lock& js, kj::StringPtr name, const jsg::JsValue& value);
  kj::String render(jsg::Lock& js, const jsg::JsValue& value);
  kj::String describeException(jsg::Lock& js, const jsg::JsValue& exception);
};

}  // namespace workerd::server
