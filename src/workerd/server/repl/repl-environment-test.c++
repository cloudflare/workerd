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

// A config with a worker that answers fetches with what it was asked, an entrypoint with props,
// and an http socket in front of the worker.
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

// A config with the process-wide settings the REPL ignores.
constexpr kj::StringPtr PROCESS_WIDE_CONFIG = R"((
  v8Flags = ["--expose-gc"],
  services = [ (name = "main", external = (address = "localhost:1")) ],
))"_kj;

// A ReplEnvironment on the test thread's event loop, whose configs are the texts above by name.
struct Fixture {
  kj::AsyncIoContext io = kj::setupAsyncIo();
  kj::Own<kj::Filesystem> fs = kj::newDiskFilesystem();
  TestEntropySource entropySource;
  ReplServerResources resources{
    .v8System = v8System,
    .timer = io.provider->getTimer(),
    .monotonicClock = kj::systemPreciseMonotonicClock(),
    .network = io.provider->getNetwork(),
    .entropySource = entropySource,
    .fs = *fs,
  };
  kj::HashMap<kj::StringPtr, kj::StringPtr> configs;
  ReplEnvironment environment;

  explicit Fixture(ReplEnvironmentOptions options = {})
      : environment(resources, KJ_BIND_METHOD(*this, loadConfig), kj::mv(options)) {
    configs.insert("worker.capnp"_kj, WORKER_CONFIG);
    configs.insert("no-sockets.capnp"_kj, NO_SOCKETS_CONFIG);
    configs.insert("broken.capnp"_kj, BROKEN_CONFIG);
    configs.insert("process-wide.capnp"_kj, PROCESS_WIDE_CONFIG);
  }

  ~Fixture() noexcept(false) {
    environment.drain().wait(io.waitScope);
  }

  kj::Array<capnp::word> loadConfig(
      kj::StringPtr path, kj::Maybe<kj::StringPtr> constName, bool experimental) {
    auto text = JSG_REQUIRE_NONNULL(configs.find(path), Error, path, ": No such file.");
    capnp::MallocMessageBuilder message;
    capnp::TextCodec().decode(text, message.initRoot<config::Config>());
    return capnp::messageToFlatArray(message);
  }

  // Evaluates `input` as it is.
  kj::String eval(kj::StringPtr input) {
    auto candidates = kj::heapArrayBuilder<ReplEnvironment::Candidate>(1);
    candidates.add(ReplEnvironment::Candidate{.source = kj::str(input), .awaited = false});
    return environment.evaluate(candidates.finish()).wait(io.waitScope);
  }

  // Evaluates `expression`, which may await, and renders what it settles to.
  kj::String evalAwait(kj::StringPtr expression) {
    auto candidates = kj::heapArrayBuilder<ReplEnvironment::Candidate>(1);
    candidates.add(ReplEnvironment::Candidate{
      .source = kj::str("(async () => (", expression, "\n))()"), .awaited = true});
    return environment.evaluate(candidates.finish()).wait(io.waitScope);
  }

  // Evaluates `input` as the REPL does an input that mentions `await`: as it is, then as an async
  // function returning its value, then as an async function's statements (cli/repl/input.rs,
  // without its rewriting of declarations).
  kj::String evalCandidates(kj::StringPtr input) {
    auto candidates = kj::heapArrayBuilder<ReplEnvironment::Candidate>(3);
    candidates.add(ReplEnvironment::Candidate{.source = kj::str(input), .awaited = false});
    candidates.add(ReplEnvironment::Candidate{
      .source = kj::str("(async () => {return (", input, "\n)})()"), .awaited = true});
    candidates.add(ReplEnvironment::Candidate{
      .source = kj::str("(async () => {", input, "\n})()"), .awaited = true});
    return environment.evaluate(candidates.finish()).wait(io.waitScope);
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

KJ_TEST("REPL tries candidates in order and awaits the awaited ones") {
  Fixture fixture;
  auto candidates = kj::heapArrayBuilder<ReplEnvironment::Candidate>(3);
  // The first does not compile; of the others, the first that does is evaluated.
  candidates.add(ReplEnvironment::Candidate{.source = kj::str("x = await f("), .awaited = false});
  candidates.add(ReplEnvironment::Candidate{
    .source = kj::str("(async () => (await Promise.resolve(42)\n))()"), .awaited = true});
  candidates.add(ReplEnvironment::Candidate{.source = kj::str("'not reached'"), .awaited = false});
  KJ_EXPECT(fixture.environment.evaluate(candidates.finish()).wait(fixture.io.waitScope) == "42");

  KJ_EXPECT_STARTS_WITH(
      fixture.evalAwait("await Promise.reject(new Error('no'))"), "Uncaught Error: no");
  // A promise that is not awaited renders as a promise.
  KJ_EXPECT(fixture.eval("Promise.resolve(1)") == "Promise { 1 }");
  // Timers run while an input is awaited.
  KJ_EXPECT(
      fixture.evalAwait("await new Promise(resolve => setTimeout(() => resolve('later'), 1))") ==
      "'later'");
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

KJ_TEST("REPL rejects invalid compatibility settings") {
  kj::AsyncIoContext io = kj::setupAsyncIo();
  auto fs = kj::newDiskFilesystem();
  TestEntropySource entropySource;
  ReplServerResources resources{
    .v8System = v8System,
    .timer = io.provider->getTimer(),
    .monotonicClock = kj::systemPreciseMonotonicClock(),
    .network = io.provider->getNetwork(),
    .entropySource = entropySource,
    .fs = *fs,
  };
  auto loader = [](kj::StringPtr, kj::Maybe<kj::StringPtr>, bool) -> kj::Array<capnp::word> {
    KJ_UNREACHABLE;
  };
  KJ_EXPECT_THROW_MESSAGE("Invalid compatibility flags",
      ReplEnvironment(resources, loader, {.compatibilityFlags = kj::arr(kj::str("no_such_flag"))}));
  KJ_EXPECT_THROW_MESSAGE("Invalid compatibility flags",
      ReplEnvironment(resources, loader, {.compatibilityDate = kj::str("not-a-date")}));
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
      "ReplServer { path: 'worker.capnp', closed: false, warnings: [ [length]: 0 ] }");
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
  KJ_EXPECT_STARTS_WITH(fixture.eval("s.service('nope')"), "Uncaught Error:");
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

}  // namespace
}  // namespace workerd::server
