// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "repl-environment.h"

#include <workerd/jsg/exception.h>
#include <workerd/jsg/setup.h>
#include <workerd/server/workerd.capnp.h>
#include <workerd/util/autogate.h>
#include <workerd/util/entropy.h>

#include <capnp/message.h>
#include <capnp/serialize-text.h>
#include <capnp/serialize.h>
#include <kj/async-io.h>
#include <kj/encoding.h>
#include <kj/filesystem.h>
#include <kj/map.h>
#include <kj/test.h>

namespace workerd::server {
namespace {

jsg::V8System v8System;

const bool autogatesInitialized = ([]() {
  util::Autogate::initAutogate({});
  return true;
})();

class TestEntropySource final: public kj::EntropySource {
 public:
  void generate(kj::ArrayPtr<kj::byte> buffer) override {
    getEntropy(buffer);
  }
};

// A config with a worker that answers fetches with what it was asked, takes scheduled and queue
// events, has an entrypoint with props, and an http socket in front of it.
constexpr kj::StringPtr WORKER_CONFIG = R"((
  services = [
    ( name = "main",
      worker = (
        compatibilityDate = "2025-01-01",
        modules = [
          ( name = "worker.js",
            esModule =
              `import { WorkerEntrypoint } from "cloudflare:workers";
              `export default {
              `  async fetch(request, env) {
              `    const url = new URL(request.url);
              `    const header = request.headers.get("x-test") ?? "none";
              `    return new Response(`${env.GREETING} ${request.method} ${url.pathname} ${header}`,
              `        { headers: { "x-reply": "yes" } });
              `  },
              `  scheduled(controller) {},
              `  queue(batch) { batch.ackAll(); },
              `};
              `export class Api extends WorkerEntrypoint {
              `  add(a, b) { return a + b; }
              `  whoami() { return this.ctx.props.user; }
              `}
          ),
        ],
        bindings = [ (name = "GREETING", text = "hello") ],
      ),
    ),
  ],
  sockets = [ (name = "http", address = "*:0", http = (), service = "main") ],
))"_kj;

// A config with a worker and no sockets, which only the REPL reaches.
constexpr kj::StringPtr NO_SOCKETS_CONFIG = R"((
  services = [
    ( name = "main",
      worker = (
        compatibilityDate = "2025-01-01",
        modules = [
          ( name = "worker.js",
            esModule = `export default { fetch() { return new Response("hello"); } };
          ),
        ],
      ),
    ),
  ],
))"_kj;

// A config the server reports errors in.
constexpr kj::StringPtr BROKEN_CONFIG = R"((
  sockets = [ (name = "http", address = "*:0", http = (), service = "missing") ],
))"_kj;

// A config whose worker's script throws as it starts.
constexpr kj::StringPtr THROWING_SCRIPT_CONFIG = R"((
  services = [
    ( name = "main",
      worker = (
        compatibilityDate = "2025-01-01",
        modules = [ (name = "worker.js", esModule = "throw new Error('broken');") ],
      ),
    ),
  ],
))"_kj;

// A config with the process-wide settings the REPL ignores.
constexpr kj::StringPtr PROCESS_WIDE_CONFIG = R"((
  v8Flags = ["--expose-gc"],
  services = [ (name = "main", external = (address = "localhost:1")) ],
))"_kj;

// A config whose worker answers every fetch with `text`.
kj::String configAnswering(kj::StringPtr text) {
  return kj::str(R"((
  services = [
    ( name = "main",
      worker = (
        compatibilityDate = "2025-01-01",
        modules = [
          ( name = "worker.js",
            esModule = "export default { fetch() { return new Response(')",
      text, R"('); } };" ),
        ],
      ),
    ),
  ],
))");
}

// A config whose worker exports a Durable Object class, Counter, stored in `storageDir`, and an
// ephemeral one, Scratch. `moduleEnd` ends the worker's module.
kj::String durableObjectsConfig(kj::StringPtr storageDir, kj::StringPtr moduleEnd = ""_kj) {
  return kj::str(R"((
  services = [
    ( name = "main",
      worker = (
        compatibilityDate = "2025-01-01",
        modules = [
          ( name = "worker.js",
            esModule =
              `import { DurableObject } from "cloudflare:workers";
              `export class Counter extends DurableObject {
              `  hits = 0;
              `  async increment() {
              `    const total = ((await this.ctx.storage.get("total")) ?? 0) + 1;
              `    await this.ctx.storage.put("total", total);
              `    return `${++this.hits}/${total}`;
              `  }
              `  id() { return this.ctx.id.toString(); }
              `}
              `export class Scratch extends DurableObject {
              `  hits = 0;
              `  hit() { return ++this.hits; }
              `}
              `export default { fetch() { return new Response("ok"); } };
              `)",
      moduleEnd, R"(
          ),
        ],
        durableObjectNamespaces = [
          (className = "Counter", uniqueKey = "counter"),
          (className = "Scratch", ephemeralLocal = void),
        ],
        durableObjectStorage = (localDisk = "storage"),
      ),
    ),
    (name = "storage", disk = (path = ")",
      storageDir, R"(", writable = true)),
  ],
))");
}

// The candidates the REPL makes of an input that mentions `await`: the input as it is, then an
// async function returning its value, then an async function's statements (cli/repl/input.rs,
// without its rewriting of declarations or its parentheses around a leading `{`).
struct TestCandidateSource final: public ReplEnvironment::CandidateSource {
  kj::Array<ReplEnvironment::Candidate> candidates(kj::StringPtr input) override {
    auto candidates = kj::heapArrayBuilder<ReplEnvironment::Candidate>(3);
    candidates.add(ReplEnvironment::Candidate{.source = kj::str(input), .awaited = false});
    candidates.add(ReplEnvironment::Candidate{
      .source = kj::str("(async () => {return (", input, "\n)})()"), .awaited = true});
    candidates.add(ReplEnvironment::Candidate{
      .source = kj::str("(async () => {", input, "\n})()"), .awaited = true});
    return candidates.finish();
  }
};

// A ReplEnvironment on the test thread's event loop, whose configs are the texts above by name.
// Its loader's watches report a change when the test calls changeWatchedFiles().
struct Fixture final: public ReplConfigLoader {
  kj::AsyncIoContext io = kj::setupAsyncIo();
  kj::Own<kj::Filesystem> fs = kj::newDiskFilesystem();
  TestEntropySource entropySource;
  ReplServerClaims claims;
  ReplServerResources resources{
    .v8System = v8System,
    .timer = io.provider->getTimer(),
    .monotonicClock = kj::systemPreciseMonotonicClock(),
    .network = io.provider->getNetwork(),
    .entropySource = entropySource,
    .fs = *fs,
    .claims = claims,
  };
  kj::HashMap<kj::String, kj::String> configs;
  // The change the last watch a server waits on would report, and the test waiting for a server to
  // wait on one.
  kj::Maybe<kj::Own<kj::PromiseFulfiller<void>>> change;
  kj::Maybe<kj::Own<kj::PromiseFulfiller<void>>> watchAwaited;

  // The close holdNextClose() holds: what it waits on, and what to tell once it does.
  struct NextClose {
    kj::Promise<void> held;
    kj::Own<kj::PromiseFulfiller<void>> waiting;
  };
  kj::Maybe<NextClose> nextClose;
  kj::Function<kj::Promise<void>()> beforeServerClose = [this]() -> kj::Promise<void> {
    KJ_IF_SOME(next, nextClose) {
      auto held = kj::mv(next.held);
      next.waiting->fulfill();
      nextClose = kj::none;
      return held;
    }
    return kj::READY_NOW;
  };

  TestCandidateSource candidateSource;
  bool drained = false;
  ReplEnvironment environment;

  explicit Fixture(ReplEnvironmentOptions options = {})
      : environment(resources, *this, candidateSource, holdingCloses(kj::mv(options))) {
    setConfig("worker.capnp", WORKER_CONFIG);
    setConfig("no-sockets.capnp", NO_SOCKETS_CONFIG);
    setConfig("broken.capnp", BROKEN_CONFIG);
    setConfig("process-wide.capnp", PROCESS_WIDE_CONFIG);
  }

  ~Fixture() noexcept(false) {
    if (!drained) {
      environment.drain().wait(io.waitScope);
    }
  }

  ReplEnvironmentOptions holdingCloses(ReplEnvironmentOptions options) {
    options.beforeServerClose = beforeServerClose;
    return options;
  }

  // Drains the environment, which ~Fixture otherwise does.
  kj::Promise<void> drain() {
    drained = true;
    return environment.drain();
  }

  struct HeldClose {
    // Resolves once the close has begun and waits.
    kj::Promise<void> waiting;
    // Lets it go on.
    kj::Own<kj::PromiseFulfiller<void>> release;
  };

  // Holds the next close of a server that the environment, or one it starts, loaded, until
  // released.
  HeldClose holdNextClose() {
    auto held = kj::newPromiseAndFulfiller<void>();
    auto waiting = kj::newPromiseAndFulfiller<void>();
    nextClose = NextClose{.held = kj::mv(held.promise), .waiting = kj::mv(waiting.fulfiller)};
    return {.waiting = kj::mv(waiting.promise), .release = kj::mv(held.fulfiller)};
  }

  void setConfig(kj::StringPtr path, kj::StringPtr text) {
    configs.upsert(kj::str(path), kj::str(text));
  }

  kj::Array<capnp::word> load(kj::StringPtr path,
      kj::Maybe<kj::StringPtr> constName,
      kj::Maybe<ReplConfigWatch&> watch) override {
    auto& text = JSG_REQUIRE_NONNULL(configs.find(path), Error, path, ": No such file.");
    capnp::MallocMessageBuilder message;
    capnp::TextCodec().decode(text, message.initRoot<config::Config>());
    return capnp::messageToFlatArray(message);
  }

  class Watch final: public ReplConfigWatch {
   public:
    explicit Watch(Fixture& fixture): fixture(fixture) {}

    kj::Promise<void> whenChanged() override {
      auto paf = kj::newPromiseAndFulfiller<void>();
      fixture.change = kj::mv(paf.fulfiller);
      KJ_IF_SOME(awaited, fixture.watchAwaited) {
        awaited->fulfill();
      }
      return kj::mv(paf.promise);
    }

   private:
    Fixture& fixture;
  };

  kj::Own<ReplConfigWatch> newWatch() override {
    return kj::heap<Watch>(*this);
  }

  // Reports a change to the files of the watched config, and waits for the reload it causes to
  // be done: the server then waits on the watch the reload made.
  void changeWatchedFiles() {
    auto paf = kj::newPromiseAndFulfiller<void>();
    watchAwaited = kj::mv(paf.fulfiller);
    KJ_ASSERT_NONNULL(change, "no server waits on a watch")->fulfill();
    change = kj::none;
    paf.promise.wait(io.waitScope);
    watchAwaited = kj::none;
  }

  // Evaluates `input` as it is.
  kj::String eval(kj::StringPtr input) {
    auto candidates = kj::heapArrayBuilder<ReplEnvironment::Candidate>(1);
    candidates.add(ReplEnvironment::Candidate{.source = kj::str(input), .awaited = false});
    return environment.evaluate(candidates.finish()).wait(io.waitScope).text;
  }

  // Evaluates `expression`, which may await, and renders what it settles to.
  kj::String evalAwait(kj::StringPtr expression) {
    auto candidates = kj::heapArrayBuilder<ReplEnvironment::Candidate>(1);
    candidates.add(ReplEnvironment::Candidate{
      .source = kj::str("(async () => (", expression, "\n))()"), .awaited = true});
    return environment.evaluate(candidates.finish()).wait(io.waitScope).text;
  }

  // Evaluates `input` as the REPL does an input that mentions `await` (TestCandidateSource).
  kj::String evalCandidates(kj::StringPtr input) {
    return environment.evaluate(candidateSource.candidates(input)).wait(io.waitScope).text;
  }

  kj::Array<kj::String> propertyNames(kj::StringPtr base) {
    return environment.propertyNames(kj::str(base)).wait(io.waitScope);
  }
};

// Evaluates `text` once: it may be an input with effects.
#define KJ_EXPECT_STARTS_WITH(text, prefix)                                                        \
  {                                                                                                \
    kj::String _text = text;                                                                       \
    KJ_EXPECT(_text.startsWith(prefix), _text);                                                    \
  }

bool contains(kj::ArrayPtr<const kj::String> names, kj::StringPtr name) {
  for (auto& n: names) {
    if (n == name) return true;
  }
  return false;
}

KJ_TEST("REPL evaluates input and keeps state between inputs") {
  Fixture fixture;
  KJ_EXPECT(fixture.eval("1 + 1") == "2");
  KJ_EXPECT(fixture.eval("'a' + 'b'") == "'ab'");
  KJ_EXPECT(fixture.eval("var a = 40; function f(x) { return x + 2; }") == "undefined");
  KJ_EXPECT(fixture.eval("f(a)") == "42");
  KJ_EXPECT(fixture.eval("({ x: [1, 2] })") == "{ x: [ 1, 2 ] }");
  KJ_EXPECT(fixture.eval("new URL('https://example.com/?q=1').searchParams.get('q')") == "'1'");
}

KJ_TEST("REPL reports exceptions and syntax errors") {
  Fixture fixture;
  KJ_EXPECT_STARTS_WITH(fixture.eval("throw new TypeError('boom')"), "Uncaught TypeError: boom");
  KJ_EXPECT(fixture.eval("throw 1") == "Uncaught 1");
  KJ_EXPECT_STARTS_WITH(fixture.eval("1 +"), "Uncaught SyntaxError");
  // The environment goes on after a failed input.
  KJ_EXPECT(fixture.eval("2") == "2");
}

KJ_TEST("REPL keeps the last result in `_` and the last exception in `_error`") {
  Fixture fixture;
  fixture.eval("6 * 7");
  KJ_EXPECT(fixture.eval("_") == "42");
  KJ_EXPECT(fixture.eval("_ + 1") == "43");
  fixture.eval("throw new RangeError('far')");
  KJ_EXPECT(fixture.eval("_error.message") == "'far'");

  // An awaited input's outcome is what its promise settles to.
  fixture.evalAwait("await Promise.resolve('later')");
  KJ_EXPECT(fixture.eval("_") == "'later'");
  fixture.evalAwait("await Promise.reject(new Error('no'))");
  KJ_EXPECT(fixture.eval("_error.message") == "'no'");

  // Once assigned, `_` keeps the assigned value.
  KJ_EXPECT(fixture.eval("_ = 'mine'") == "'mine'");
  fixture.eval("1");
  KJ_EXPECT(fixture.eval("_") == "'mine'");
  // `_error` still follows exceptions.
  fixture.eval("throw 2");
  KJ_EXPECT(fixture.eval("_error") == "2");
}

KJ_TEST("REPL says whether an input threw") {
  Fixture fixture;
  auto threw = [&](kj::StringPtr source, bool awaited) {
    auto candidates = kj::heapArrayBuilder<ReplEnvironment::Candidate>(1);
    candidates.add(ReplEnvironment::Candidate{.source = kj::str(source), .awaited = awaited});
    return fixture.environment.evaluate(candidates.finish()).wait(fixture.io.waitScope).threw;
  };
  KJ_EXPECT(!threw("1 + 1", false));
  KJ_EXPECT(threw("null.x", false));
  KJ_EXPECT(threw("1 +", false));
  KJ_EXPECT(!threw("Promise.resolve(1)", true));
  KJ_EXPECT(threw("Promise.reject(new Error('no'))", true));
  // A result that renders as "Uncaught ..." is a result.
  KJ_EXPECT(fixture.eval("class Uncaught {}; new Uncaught()") == "Uncaught {}");
  KJ_EXPECT(!threw("new Uncaught()", false));
}

KJ_TEST("REPL tries candidates in order and awaits the awaited ones") {
  Fixture fixture;
  auto candidates = kj::heapArrayBuilder<ReplEnvironment::Candidate>(3);
  // The first does not compile; of the others, the first that does is evaluated.
  candidates.add(ReplEnvironment::Candidate{.source = kj::str("x = await f("), .awaited = false});
  candidates.add(ReplEnvironment::Candidate{
    .source = kj::str("(async () => (await Promise.resolve(42)\n))()"), .awaited = true});
  candidates.add(ReplEnvironment::Candidate{.source = kj::str("'not reached'"), .awaited = false});
  KJ_EXPECT(
      fixture.environment.evaluate(candidates.finish()).wait(fixture.io.waitScope).text == "42");

  KJ_EXPECT_STARTS_WITH(
      fixture.evalAwait("await Promise.reject(new Error('no'))"), "Uncaught Error: no");
  // A promise that is not awaited renders as a promise.
  KJ_EXPECT(fixture.eval("Promise.resolve(1)") == "Promise { 1 }");
  // Timers run while an input is awaited.
  KJ_EXPECT(
      fixture.evalAwait("await new Promise(resolve => setTimeout(() => resolve('later'), 1))") ==
      "'later'");
}

KJ_TEST("REPL reports the syntax error of an input, not of its parentheses") {
  Fixture fixture;
  auto evaluate = [&](kj::StringPtr input) {
    auto candidates = kj::heapArrayBuilder<ReplEnvironment::Candidate>(2);
    candidates.add(ReplEnvironment::Candidate{
      .source = kj::str("(", input, "\n)"), .awaited = false, .parenthesized = true});
    candidates.add(ReplEnvironment::Candidate{.source = kj::str(input), .awaited = false});
    return fixture.environment.evaluate(candidates.finish()).wait(fixture.io.waitScope).text;
  };
  // The parentheses make an object literal of what would be a block.
  KJ_EXPECT(evaluate("{ a: 1, b: 2 }") == "{ a: 1, b: 2 }");
  KJ_EXPECT(evaluate("{ let x = 1; x }") == "1");
  // In parentheses, this is "Unexpected identifier 'x'".
  KJ_EXPECT(evaluate("{ let x = ; }") == "Uncaught SyntaxError: Unexpected token ';'");
}

KJ_TEST("REPL awaits an input that a script would read as using a name `await`") {
  Fixture fixture;
  KJ_EXPECT(fixture.evalCandidates("await (Promise.resolve(1))") == "1");
  KJ_EXPECT(fixture.evalCandidates("await [Promise.resolve(2)]") == "[ Promise { 2 } ]");
  KJ_EXPECT(fixture.evalCandidates("await - 3") == "-3");
  KJ_EXPECT(fixture.evalCandidates("if (true) { x = await (Promise.resolve(4)) }") == "undefined");
  KJ_EXPECT(fixture.eval("x") == "4");

  // An input that awaits only in async functions runs as it is, so its declarations are global.
  KJ_EXPECT(fixture.evalCandidates("async function f() { return await (Promise.resolve(5)) }") ==
      "undefined");
  KJ_EXPECT(fixture.evalAwait("await f()") == "5");
  KJ_EXPECT(
      fixture.evalCandidates("const g = async () => await (Promise.resolve(6))") == "undefined");
  KJ_EXPECT(fixture.evalAwait("await g()") == "6");

  // An input that declares a name `await` runs as it is: nothing that awaits compiles.
  KJ_EXPECT(fixture.evalCandidates("var await = (n) => n + 1; await (6)") == "7");
}

KJ_TEST("REPL enables nodejs_compat unless the flags take a position on it") {
  // A date before nodejs_compat was the default.
  {
    Fixture fixture({.compatibilityDate = kj::str("2024-01-01")});
    KJ_EXPECT(fixture.evalAwait("(await import('node:path')).join('a', 'b')") == "'a/b'");
  }
  {
    Fixture fixture({.compatibilityDate = kj::str("2024-01-01"),
      .compatibilityFlags = kj::arr(kj::str("no_nodejs_compat"))});
    KJ_EXPECT_STARTS_WITH(fixture.evalAwait("(await import('node:path')).join('a', 'b')"),
        "Uncaught Error: Module not found: node:path");
  }
}

// How many times the REPL's JavaScript sees the failure of an RPC call through a stale Fetcher,
// awaited at once, as an unhandled rejection. The REPL prints each as "Uncaught (in promise)",
// besides the input's failure.
kj::String unhandledStaleCalls(Fixture& fixture) {
  fixture.setConfig("reload.capnp", configAnswering("one"));
  fixture.evalAwait("s = await workerd.loadConfig('reload.capnp')");
  fixture.eval("unhandled = 0; addEventListener('unhandledrejection', () => ++unhandled), 0");
  fixture.eval("f = s.service('main'), 0");
  fixture.evalAwait("await s.reload()");
  // The call fails at once, and hands back a promise already rejected.
  KJ_EXPECT_STARTS_WITH(fixture.evalAwait("await f.hello()"),
      "Uncaught Error: The server for reload.capnp was reloaded.");
  return fixture.evalAwait("await new Promise((resolve) => setTimeout(resolve, 0)), unhandled");
}

KJ_TEST("REPL reports an awaited call that fails at once as the input's failure alone") {
  // A date before unhandled_rejection_after_microtask_checkpoint was the default.
  {
    Fixture fixture({.compatibilityDate = kj::str("2025-06-01")});
    KJ_EXPECT(unhandledStaleCalls(fixture) == "0");
  }
  // Unless the flags take a position on it.
  {
    Fixture fixture({.compatibilityDate = kj::str("2025-06-01"),
      .compatibilityFlags = kj::arr(kj::str("no_unhandled_rejection_after_microtask_checkpoint"))});
    KJ_EXPECT(unhandledStaleCalls(fixture) == "1");
  }
}

KJ_TEST("REPL rejects invalid compatibility settings") {
  kj::AsyncIoContext io = kj::setupAsyncIo();
  auto fs = kj::newDiskFilesystem();
  TestEntropySource entropySource;
  ReplServerClaims claims;
  ReplServerResources resources{
    .v8System = v8System,
    .timer = io.provider->getTimer(),
    .monotonicClock = kj::systemPreciseMonotonicClock(),
    .network = io.provider->getNetwork(),
    .entropySource = entropySource,
    .fs = *fs,
    .claims = claims,
  };
  struct: public ReplConfigLoader {
    kj::Array<capnp::word> load(
        kj::StringPtr, kj::Maybe<kj::StringPtr>, kj::Maybe<ReplConfigWatch&>) override {
      KJ_UNREACHABLE;
    }
    kj::Own<ReplConfigWatch> newWatch() override {
      KJ_UNREACHABLE;
    }
  } loader;
  TestCandidateSource candidates;
  KJ_EXPECT_THROW_MESSAGE("Invalid compatibility flags",
      ReplEnvironment(
          resources, loader, candidates, {.compatibilityFlags = kj::arr(kj::str("no_such_flag"))}));
  KJ_EXPECT_THROW_MESSAGE("Invalid compatibility flags",
      ReplEnvironment(resources, loader, candidates, {.compatibilityDate = kj::str("not-a-date")}));
}

KJ_TEST("REPL lists property names for completion") {
  Fixture fixture;
  auto globals = fixture.propertyNames("");
  KJ_EXPECT(contains(globals, "workerd"));
  KJ_EXPECT(contains(globals, "fetch"));

  auto control = fixture.propertyNames("workerd");
  KJ_EXPECT(contains(control, "loadConfig"));
  KJ_EXPECT(contains(control, "servers"));

  fixture.eval("var thing = { inner: { deep: 1 } }");
  auto inner = fixture.propertyNames("thing.inner");
  KJ_EXPECT(contains(inner, "deep"));
  // Inherited names count.
  KJ_EXPECT(contains(inner, "hasOwnProperty"));

  // Nothing for what is not an identifier chain, what does not evaluate, or null.
  KJ_EXPECT(fixture.propertyNames("f()").size() == 0);
  KJ_EXPECT(fixture.propertyNames("noSuchName").size() == 0);
  fixture.eval("var nothing = null");
  KJ_EXPECT(fixture.propertyNames("nothing").size() == 0);
}

KJ_TEST("REPL loads a config and calls its services and sockets") {
  Fixture fixture;
  KJ_EXPECT(fixture.evalAwait("s = await workerd.loadConfig('worker.capnp')") ==
      "ReplServer {\n"
      "  path: 'worker.capnp',\n"
      "  closed: false,\n"
      "  watching: false,\n"
      "  warnings: [ [length]: 0 ]\n"
      "}");
  KJ_EXPECT(fixture.eval("workerd.servers.length") == "1");

  // Headers cross in both directions, though the REPL and the server key them differently.
  KJ_EXPECT(
      fixture.evalAwait(
          "(r = await s.service('main').fetch('http://x/a', "
          "{ method: 'POST', headers: { 'x-test': 'sent' } })).headers.get('x-reply')") == "'yes'");
  KJ_EXPECT(fixture.evalAwait("await r.text()") == "'hello POST /a sent'");

  KJ_EXPECT(fixture.evalAwait("await s.service('main', { entrypoint: 'Api' }).add(2, 3)") == "5");
  KJ_EXPECT(
      fixture.evalAwait(
          "await s.service('main', { entrypoint: 'Api', props: { user: 'ada' } }).whoami()") ==
      "'ada'");

  KJ_EXPECT(fixture.evalAwait("await (await s.socket('http').fetch('http://x/b')).text()") ==
      "'hello GET /b none'");

  KJ_EXPECT_STARTS_WITH(
      fixture.eval("s.socket('nope')"), "Uncaught Error: The config has no socket named \"nope\".");
  KJ_EXPECT_STARTS_WITH(fixture.eval("s.service('nope')"),
      "Uncaught Error: The config has no service named \"nope\".");
}

KJ_TEST("REPL calls a service's scheduled() and queue() handlers with --experimental") {
  {
    Fixture fixture({.experimental = true});
    fixture.evalAwait("s = await workerd.loadConfig('worker.capnp')");
    KJ_EXPECT(fixture.evalAwait(
                  "(await s.service('main').scheduled({ cron: '* * * * *' })).outcome") == "'ok'");
    KJ_EXPECT(
        fixture.evalAwait(
            "(await s.service('main').queue('q', "
            "[{ id: 'm', timestamp: new Date(), attempts: 1, body: 'hi' }])).ackAll") == "true");
  }
  {
    // Naming the flag as well is not naming it twice.
    Fixture fixture({.compatibilityFlags = kj::arr(kj::str("service_binding_extra_handlers")),
      .experimental = true});
    fixture.evalAwait("s = await workerd.loadConfig('worker.capnp')");
    KJ_EXPECT(fixture.evalAwait(
                  "(await s.service('main').scheduled({ cron: '* * * * *' })).outcome") == "'ok'");
  }
  {
    Fixture fixture;
    fixture.evalAwait("s = await workerd.loadConfig('worker.capnp')");
    // Without the flag, `scheduled` is an RPC call to the handler, which returns nothing.
    KJ_EXPECT(fixture.evalAwait("await s.service('main').scheduled({ cron: '* * * * *' })") ==
        "undefined");
  }
}

KJ_TEST("REPL calls the services of a config with no sockets") {
  Fixture fixture;
  fixture.evalAwait("s = await workerd.loadConfig('no-sockets.capnp')");
  // Lets the run of a server that has nothing to listen on end, if it does.
  fixture.evalAwait("await scheduler.wait(1)");
  KJ_EXPECT(
      fixture.evalAwait("await (await s.service('main').fetch('http://x/')).text()") == "'hello'");
}

KJ_TEST("REPL revokes what a closed server handed out") {
  Fixture fixture;
  fixture.evalAwait("s = await workerd.loadConfig('worker.capnp')");
  fixture.eval("main = s.service('main'); api = s.service('main', { entrypoint: 'Api' })");
  // A response whose body is never read keeps its request open.
  KJ_EXPECT(fixture.evalAwait("(unread = await main.fetch('http://x/')).status") == "200");

  KJ_EXPECT(fixture.evalAwait("await s.close()") == "undefined");
  KJ_EXPECT(fixture.eval("s.closed") == "true");
  KJ_EXPECT(fixture.eval("workerd.servers.length") == "0");
  // Closing again is fine.
  KJ_EXPECT(fixture.evalAwait("await s.close()") == "undefined");

  auto closed = "Uncaught Error: The server for worker.capnp was closed."_kj;
  KJ_EXPECT_STARTS_WITH(fixture.evalAwait("await unread.text()"), closed);
  KJ_EXPECT_STARTS_WITH(fixture.evalAwait("await main.fetch('http://x/')"), closed);
  KJ_EXPECT_STARTS_WITH(fixture.evalAwait("await api.add(1, 2)"), closed);
  KJ_EXPECT_STARTS_WITH(fixture.eval("s.service('main')"), closed);
}

// The directory `name` in the test's temporary directory, empty: without what a run before this one
// stored.
kj::String emptyDirectory(Fixture& fixture, kj::StringPtr name) {
  const char* tmpdir = getenv("TEST_TMPDIR");
  auto directory = kj::str(tmpdir == nullptr ? "/tmp" : tmpdir, "/", name);
  auto path = fixture.fs->getCurrentPath().evalNative(directory);
  fixture.fs->getRoot().tryRemove(path);
  fixture.fs->getRoot().openSubdir(path, kj::WriteMode::CREATE | kj::WriteMode::CREATE_PARENT);
  return directory;
}

KJ_TEST("REPL reaches a config's Durable Objects and evicts them") {
  // For the ephemeral class.
  Fixture fixture({.experimental = true});
  auto storageDir = emptyDirectory(fixture, "repl-durable-objects");
  fixture.setConfig("durable-objects.capnp", durableObjectsConfig(storageDir));
  KJ_EXPECT_STARTS_WITH(
      fixture.evalAwait("s = await workerd.loadConfig('durable-objects.capnp')"), "ReplServer");

  fixture.eval("alice = s.durableObject('main', 'Counter', 'alice')");
  KJ_EXPECT(fixture.evalAwait("await alice.increment()") == "'1/1'");
  KJ_EXPECT(fixture.evalAwait("await alice.increment()") == "'2/2'");
  // The same object, by name or by id; another name names another.
  KJ_EXPECT(
      fixture.evalAwait(
          "await s.durableObject('main', 'Counter', { name: 'alice' }).increment()") == "'3/3'");
  KJ_EXPECT(fixture.evalAwait("await s.durableObject('main', 'Counter', "
                              "{ id: await alice.id() }).increment()") == "'4/4'");
  KJ_EXPECT(
      fixture.evalAwait("await s.durableObject('main', 'Counter', 'bob').increment()") == "'1/1'");

  // Evicted, the object starts again from its storage.
  KJ_EXPECT(fixture.evalAwait("await workerd.evict(alice)") == "undefined");
  KJ_EXPECT(fixture.evalAwait("await alice.increment()") == "'1/5'");
  KJ_EXPECT(
      fixture.evalAwait("await workerd.evict(alice, { webSockets: 'close' })") == "undefined");
  KJ_EXPECT_STARTS_WITH(fixture.evalAwait("await workerd.evict(alice)"), "Uncaught Error:");
  KJ_EXPECT_STARTS_WITH(fixture.evalAwait("await workerd.evict(s.service('main'))"),
      "Uncaught Error: evict() can only be used on a Durable Object stub.");

  // An ephemeral object has a name and no id.
  KJ_EXPECT(fixture.evalAwait("await s.durableObject('main', 'Scratch', 'x').hit()") == "1");
  KJ_EXPECT(fixture.evalAwait("await s.durableObject('main', 'Scratch', 'x').hit()") == "2");
  KJ_EXPECT_STARTS_WITH(fixture.eval("s.durableObject('main', 'Scratch', { id: 'x' })"),
      "Uncaught TypeError: The objects of the ephemeral Durable Object class \"Scratch\" have "
      "names, not ids.");

  KJ_EXPECT_STARTS_WITH(fixture.eval("s.durableObject('main', 'Counter', { id: 'x' })"),
      "Uncaught TypeError: Invalid Durable Object ID: must be 64 hex digits");
  KJ_EXPECT_STARTS_WITH(fixture.eval("s.durableObject('main', 'Counter', {})"),
      "Uncaught TypeError: A Durable Object is named by one of a name and an id.");
  KJ_EXPECT_STARTS_WITH(fixture.eval("s.durableObject('main', 'Nope', 'x')"),
      "Uncaught Error: Worker does not export a Durable Object class named \"Nope\"");
  KJ_EXPECT_STARTS_WITH(fixture.eval("s.durableObject('nope', 'Counter', 'x')"),
      "Uncaught Error: The config has no service named \"nope\".");

  fixture.evalAwait("await s.close()");
  auto closed = "Uncaught Error: The server for durable-objects.capnp was closed."_kj;
  KJ_EXPECT_STARTS_WITH(fixture.evalAwait("await alice.increment()"), closed);
  KJ_EXPECT_STARTS_WITH(fixture.evalAwait("await workerd.evict(alice)"), closed);
}

KJ_TEST("REPL starts environments with compatibility settings of their own") {
  Fixture fixture({.compatibilityDate = kj::str("2025-01-01")});
  // `navigator` came with 2022-03-21.
  fixture.evalAwait("old = await workerd.environment({ compatibilityDate: '2022-01-01' })");
  KJ_EXPECT(fixture.eval("old") ==
      "ReplEnvironmentHandle {\n"
      "  compatibilityDate: '2022-01-01',\n"
      "  compatibilityFlags: [ [length]: 0 ],\n"
      "  closed: false\n"
      "}");
  KJ_EXPECT(fixture.evalAwait("await old.eval('typeof navigator')") == "\"'undefined'\"");
  KJ_EXPECT(fixture.eval("typeof navigator") == "'object'");

  // Flags as well, and inputs that await; an environment keeps its state between inputs.
  fixture.evalAwait("flagged = await workerd.environment({ compatibilityDate: '2022-01-01', "
                    "compatibilityFlags: ['global_navigator'] })");
  KJ_EXPECT(
      fixture.evalAwait("await flagged.eval('x = await Promise.resolve(typeof navigator)')") ==
      "\"'object'\"");
  KJ_EXPECT(fixture.evalAwait("await flagged.eval('x')") == "\"'object'\"");
  KJ_EXPECT_STARTS_WITH(fixture.evalAwait("await flagged.eval('throw new TypeError(`no`)')"),
      "'Uncaught TypeError: no");

  // Today's date without one.
  fixture.evalAwait("today = await workerd.environment()");
  KJ_EXPECT(
      fixture.eval("today.compatibilityDate === new Date().toISOString().slice(0, 10)") == "true");

  KJ_EXPECT_STARTS_WITH(
      fixture.evalAwait("await workerd.environment({ compatibilityFlags: ['no_such_flag'] })"),
      "Uncaught Error: Invalid compatibility flags:");

  // An evaluation the close cuts short fails, and so does every later one.
  fixture.eval("pending = old.eval('await new Promise(() => {})')");
  KJ_EXPECT(fixture.evalAwait("await old.close()") == "undefined");
  KJ_EXPECT(fixture.eval("old.closed") == "true");
  auto closed = "Uncaught Error: The environment was closed."_kj;
  KJ_EXPECT_STARTS_WITH(fixture.evalAwait("await pending"), closed);
  KJ_EXPECT_STARTS_WITH(fixture.evalAwait("await old.eval('1')"), closed);
  KJ_EXPECT(fixture.evalAwait("await old.close()") == "undefined");
}

constexpr kj::StringPtr FETCH_MAIN = "await (await s.service('main').fetch('http://x/')).text()"_kj;

KJ_TEST("REPL reloads a config in place") {
  Fixture fixture;
  fixture.setConfig("reload.capnp", configAnswering("one"));
  KJ_EXPECT(
      fixture.evalAwait("(s = await workerd.loadConfig('reload.capnp')).watching") == "false");
  fixture.eval("main = s.service('main')");
  KJ_EXPECT(fixture.evalAwait(FETCH_MAIN) == "'one'");

  fixture.setConfig("reload.capnp", configAnswering("two"));
  KJ_EXPECT(fixture.evalAwait("(await s.reload()) === s") == "true");
  KJ_EXPECT(fixture.evalAwait(FETCH_MAIN) == "'two'");
  KJ_EXPECT(fixture.eval("workerd.servers.length") == "1");
  // What the replaced server handed out fails, saying how to get its replacement.
  KJ_EXPECT_STARTS_WITH(fixture.evalAwait("await main.fetch('http://x/')"),
      "Uncaught Error: The server for reload.capnp was reloaded. Get a new one from its "
      "service(), socket() or durableObject().");

  // A config that no longer loads leaves the server as it was.
  fixture.configs.erase("reload.capnp"_kj);
  KJ_EXPECT_STARTS_WITH(
      fixture.evalAwait("await s.reload()"), "Uncaught Error: reload.capnp: No such file.");
  KJ_EXPECT(fixture.evalAwait(FETCH_MAIN) == "'two'");

  // One whose server doesn't start leaves nothing serving, since the old server closes first,
  // until a reload starts one.
  auto noServer = "Uncaught Error: Nothing serves reload.capnp: its last reload failed to start "
                  "the new server. Fix the config and reload()."_kj;
  fixture.setConfig("reload.capnp", THROWING_SCRIPT_CONFIG);
  KJ_EXPECT_STARTS_WITH(fixture.evalAwait("await s.reload()"),
      "Uncaught Error: reload.capnp did not reload, and nothing serves it: Failed to load "
      "reload.capnp:");
  KJ_EXPECT_STARTS_WITH(fixture.eval("s.service('main')"), noServer);
  KJ_EXPECT_STARTS_WITH(fixture.eval("s.socket('http')"), noServer);
  KJ_EXPECT(fixture.eval("[s.closed, workerd.servers.length]") == "[ false, 1 ]");
  fixture.configs.erase("reload.capnp"_kj);
  KJ_EXPECT_STARTS_WITH(fixture.evalAwait("await s.reload()"),
      "Uncaught Error: reload.capnp did not reload, and nothing serves it: reload.capnp: No such "
      "file.");
  fixture.setConfig("reload.capnp", configAnswering("three"));
  KJ_EXPECT(fixture.evalAwait("(await s.reload()) === s") == "true");
  KJ_EXPECT(fixture.evalAwait(FETCH_MAIN) == "'three'");

  fixture.evalAwait("await s.close()");
  KJ_EXPECT_STARTS_WITH(fixture.evalAwait("await s.reload()"),
      "Uncaught Error: The server for reload.capnp was closed.");
}

KJ_TEST("REPL reloads a watched config when a file it was loaded from changes") {
  Fixture fixture;
  fixture.setConfig("reload.capnp", configAnswering("one"));
  KJ_EXPECT(
      fixture.evalAwait(
          "(s = await workerd.loadConfig('reload.capnp', { watch: true })).watching") == "true");
  KJ_EXPECT(fixture.evalAwait(FETCH_MAIN) == "'one'");
  fixture.setConfig("reload.capnp", configAnswering("two"));
  fixture.changeWatchedFiles();
  KJ_EXPECT(fixture.evalAwait(FETCH_MAIN) == "'two'");

  // A change that leaves the config unloadable leaves the server running, and one that breaks its
  // server leaves nothing serving. The change that fixes it reloads it.
  fixture.configs.erase("reload.capnp"_kj);
  fixture.changeWatchedFiles();
  KJ_EXPECT(fixture.evalAwait(FETCH_MAIN) == "'two'");
  fixture.setConfig("reload.capnp", BROKEN_CONFIG);
  fixture.changeWatchedFiles();
  KJ_EXPECT_STARTS_WITH(
      fixture.evalAwait(FETCH_MAIN), "Uncaught Error: Nothing serves reload.capnp");
  fixture.setConfig("reload.capnp", configAnswering("three"));
  fixture.changeWatchedFiles();
  KJ_EXPECT(fixture.evalAwait(FETCH_MAIN) == "'three'");

  // A reload asked for by hand watches on.
  fixture.setConfig("reload.capnp", configAnswering("four"));
  fixture.evalAwait("await s.reload()");
  fixture.setConfig("reload.capnp", configAnswering("five"));
  fixture.changeWatchedFiles();
  KJ_EXPECT(fixture.evalAwait(FETCH_MAIN) == "'five'");

  fixture.evalAwait("await s.close()");
  KJ_EXPECT(fixture.eval("s.watching") == "false");
}

KJ_TEST("REPL closes the server a reload replaces before the new one serves") {
  Fixture fixture;
  fixture.setConfig("reload.capnp", configAnswering("one"));
  fixture.evalAwait("s = await workerd.loadConfig('reload.capnp')");
  fixture.setConfig("reload.capnp", configAnswering("two"));
  auto close = fixture.holdNextClose();
  KJ_EXPECT(fixture.eval("reloaded = s.reload(), 'reloading'") == "'reloading'");
  close.waiting.wait(fixture.io.waitScope);
  // The old server is closing, and the new one isn't serving yet.
  KJ_EXPECT_STARTS_WITH(fixture.eval("s.service('main')"),
      "Uncaught Error: The server for reload.capnp is reloading.");
  close.release->fulfill();
  KJ_EXPECT(fixture.evalAwait("(await reloaded) === s") == "true");
  KJ_EXPECT(fixture.evalAwait(FETCH_MAIN) == "'two'");
}

KJ_TEST("REPL refuses a config whose Durable Objects another server stores") {
  Fixture fixture({.experimental = true});
  auto storageDir = emptyDirectory(fixture, "repl-claimed-durable-objects");
  fixture.setConfig("durable-objects.capnp", durableObjectsConfig(storageDir));
  fixture.setConfig("again.capnp", durableObjectsConfig(storageDir));
  fixture.evalAwait("s = await workerd.loadConfig('durable-objects.capnp')");
  auto refused = kj::str("Uncaught Error: again.capnp stores Durable Objects in ",
      fixture.fs->getCurrentPath().evalNative(storageDir).toNativeString(true),
      "/counter, as the server for durable-objects.capnp does. Close that server first.");
  KJ_EXPECT_STARTS_WITH(fixture.evalAwait("await workerd.loadConfig('again.capnp')"), refused);
  KJ_EXPECT(fixture.eval("workerd.servers.length") == "1");
  // As is a load in another environment. Its eval() returns the message as a string, which renders
  // as a literal that escapes backslashes, such as a Windows path's, as encodeCEscape() does.
  fixture.evalAwait("e = await workerd.environment()");
  KJ_EXPECT_STARTS_WITH(
      fixture.evalAwait("await e.eval(\"await workerd.loadConfig('again.capnp')\")"),
      kj::str("'", kj::encodeCEscape(refused)));

  // A reload claims what the server it replaces held.
  KJ_EXPECT(fixture.evalAwait("(await s.reload()) === s") == "true");
  KJ_EXPECT_STARTS_WITH(fixture.evalAwait("await workerd.loadConfig('again.capnp')"), refused);

  // A reload whose server doesn't start leaves nothing holding them.
  fixture.setConfig(
      "durable-objects.capnp", durableObjectsConfig(storageDir, "throw new Error('broken');"));
  KJ_EXPECT_STARTS_WITH(fixture.evalAwait("await s.reload()"),
      "Uncaught Error: durable-objects.capnp did not reload, and nothing serves it:");
  KJ_EXPECT_STARTS_WITH(fixture.evalAwait("t = await workerd.loadConfig('again.capnp')"),
      "ReplServer {\n  path: 'again.capnp',");
  fixture.evalAwait("await t.close()");
  fixture.setConfig("durable-objects.capnp", durableObjectsConfig(storageDir));
  KJ_EXPECT(fixture.evalAwait("(await s.reload()) === s") == "true");
  KJ_EXPECT_STARTS_WITH(fixture.evalAwait("await workerd.loadConfig('again.capnp')"), refused);

  // Once that server has closed, another may store them.
  fixture.evalAwait("await s.close()");
  KJ_EXPECT_STARTS_WITH(fixture.evalAwait("t = await workerd.loadConfig('again.capnp')"),
      "ReplServer {\n  path: 'again.capnp',");
  KJ_EXPECT(fixture.evalAwait("await t.durableObject('main', 'Counter', 'alice').increment()") ==
      "'1/1'");
}

// A config whose socket "udp" listens for UDP datagrams on `address`.
kj::String udpConfig(kj::StringPtr address) {
  return kj::str(R"((
  services = [
    ( name = "main",
      worker = (
        compatibilityDate = "2025-01-01",
        modules = [ (name = "worker.js", esModule = "export default { connect() {} };") ],
      ),
    ),
  ],
  sockets = [ (name = "udp", address = ")",
      address, R"(", udp = (), service = "main") ],
))");
}

KJ_TEST("REPL refuses a config a UDP address another server listens on") {
  Fixture fixture;
  // A port that was free a moment ago.
  auto port = fixture.io.provider->getNetwork()
                  .parseAddress("127.0.0.1")
                  .wait(fixture.io.waitScope)
                  ->bindDatagramPort()
                  ->getPort();
  auto address = kj::str("127.0.0.1:", port);
  fixture.setConfig("udp.capnp", udpConfig(address));
  fixture.evalAwait("s = await workerd.loadConfig('udp.capnp')");
  auto refused = kj::str("Uncaught Error: udp.capnp listens for UDP datagrams on ", address,
      ", as the server for udp.capnp does. Close that server first.");
  KJ_EXPECT_STARTS_WITH(fixture.evalAwait("await workerd.loadConfig('udp.capnp')"), refused);
  KJ_EXPECT(fixture.eval("workerd.servers.length") == "1");

  // A reload claims what the server it replaces held.
  KJ_EXPECT(fixture.evalAwait("(await s.reload()) === s") == "true");
  KJ_EXPECT_STARTS_WITH(fixture.evalAwait("await workerd.loadConfig('udp.capnp')"), refused);

  // Once that server has closed, another may listen there.
  fixture.evalAwait("await s.close()");
  KJ_EXPECT_STARTS_WITH(fixture.evalAwait("await workerd.loadConfig('udp.capnp')"),
      "ReplServer {\n  path: 'udp.capnp',");

  // On port 0, or none, each server listens on a port of the system's choosing.
  for (const auto& unclaimed: {"127.0.0.1:0"_kj, "127.0.0.1"_kj}) {
    fixture.setConfig("any-port.capnp", udpConfig(unclaimed));
    for (auto i KJ_UNUSED: kj::zeroTo(2)) {
      KJ_EXPECT_STARTS_WITH(fixture.evalAwait("await workerd.loadConfig('any-port.capnp')"),
          "ReplServer {\n  path: 'any-port.capnp',");
    }
  }
}

KJ_TEST("REPL reports configs that do not load") {
  Fixture fixture;
  KJ_EXPECT_STARTS_WITH(fixture.evalAwait("await workerd.loadConfig('missing.capnp')"),
      "Uncaught Error: missing.capnp: No such file.");
  KJ_EXPECT_STARTS_WITH(fixture.evalAwait("await workerd.loadConfig('broken.capnp')"),
      "Uncaught Error: Failed to load broken.capnp:\n  ");
  KJ_EXPECT(fixture.eval("workerd.servers.length") == "0");

  auto warnings = fixture.evalAwait("(await workerd.loadConfig('process-wide.capnp')).warnings");
  KJ_EXPECT(warnings.contains("v8Flags are ignored"), warnings);
}

KJ_TEST("REPL closes loaded servers when it drains") {
  Fixture fixture;
  fixture.evalAwait("s = await workerd.loadConfig('worker.capnp')");
  fixture.evalAwait("t = await workerd.loadConfig('worker.capnp')");
  KJ_EXPECT(fixture.eval("workerd.servers.length") == "2");
  // ~Fixture drains, closing both; a server destroyed while open would fail an assertion.
}

KJ_TEST("REPL closes the server of a load still under way when it drains") {
  Fixture fixture;
  fixture.evalAwait("s = await workerd.loadConfig('worker.capnp')");
  // Not awaited: ~Fixture drains while the load runs, and closes its server once it has started.
  KJ_EXPECT(fixture.eval("workerd.loadConfig('worker.capnp'), 'loading'") == "'loading'");
}

KJ_TEST("REPL closes a server at once, whatever the config's shutdownDelayMs") {
  Fixture fixture;
  // WORKER_CONFIG, with a delay that ~Fixture can wait out if the close does not skip it.
  fixture.setConfig("delayed.capnp", kj::str("(shutdownDelayMs = 20000,", WORKER_CONFIG.slice(1)));
  fixture.evalAwait("s = await workerd.loadConfig('delayed.capnp')");
  // Answered once the run serves its listeners. A close before then drains at once anyway, as
  // for a signal that comes while the server starts.
  KJ_EXPECT(fixture.evalAwait("await (await s.socket('http').fetch('http://x/')).text()") ==
      "'hello GET / none'");
  auto candidates = kj::heapArrayBuilder<ReplEnvironment::Candidate>(1);
  candidates.add(ReplEnvironment::Candidate{
    .source = kj::str("(async () => (await s.close()\n))()"), .awaited = true});
  auto closing = fixture.environment.evaluate(candidates.finish());
  auto closed = fixture.resources.timer.timeoutAfter(5 * kj::SECONDS, kj::mv(closing));
  KJ_EXPECT(closed.wait(fixture.io.waitScope).text == "undefined");
}

// The drain that `fixture` starts once `close`'s close is held and the environment has let go of
// what it closes: the drain must wait for the close, and then finish.
void expectDrainWaitsFor(Fixture& fixture, Fixture::HeldClose close) {
  auto draining = fixture.drain();
  KJ_EXPECT(!draining.poll(fixture.io.waitScope), "the drain did not wait for the close");
  close.release->fulfill();
  draining.wait(fixture.io.waitScope);
}

KJ_TEST("REPL drains a server whose close a later load found still under way") {
  Fixture fixture;
  fixture.evalAwait("s = await workerd.loadConfig('worker.capnp')");
  auto close = fixture.holdNextClose();
  KJ_EXPECT(fixture.eval("s.close(), 'closing'") == "'closing'");
  close.waiting.wait(fixture.io.waitScope);
  // A load lets go of the servers that have closed.
  fixture.evalAwait("t = await workerd.loadConfig('worker.capnp')");
  expectDrainWaitsFor(fixture, kj::mv(close));
}

KJ_TEST("REPL drains an environment whose close a later start found still under way") {
  Fixture fixture;
  fixture.evalAwait("e = await workerd.environment()");
  KJ_EXPECT_STARTS_WITH(
      fixture.evalAwait("await e.eval(\"s = await workerd.loadConfig('worker.capnp')\")"),
      "'ReplServer {");
  auto close = fixture.holdNextClose();
  // Closing the environment closes its server, whose close waits.
  KJ_EXPECT(fixture.eval("e.close(), 'closing'") == "'closing'");
  close.waiting.wait(fixture.io.waitScope);
  // A start lets go of the environments that have closed.
  fixture.evalAwait("f = await workerd.environment()");
  expectDrainWaitsFor(fixture, kj::mv(close));
}

}  // namespace
}  // namespace workerd::server
