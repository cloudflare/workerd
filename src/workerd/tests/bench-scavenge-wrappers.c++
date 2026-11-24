// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Times a minor GC over young wrappers of API types. For each wrapper that a C++ object holds,
// V8 decides whether the wrapper holds anything a fresh one wouldn't; if it doesn't and nothing in
// JavaScript reaches it, V8 drops it. Most of these types have their properties on the instance
// at compatibility dates before workers_api_getters_setters_on_prototype, which V8 has to check
// against the type's templates for every such wrapper, live or dead.

#include "bench-tools.h"
#include "test-fixture.h"

namespace workerd {
namespace {

enum class Properties { ON_INSTANCE, ON_PROTOTYPE };
enum class Wrappers { DEAD, LIVE };

// Returns a function that makes `count` of each type and returns them if `keep`.
constexpr kj::StringPtr MAKE_WRAPPERS = R"((count, keep) => {
  const kept = [];
  for (let i = 0; i < count; i++) {
    const objects = [
      new Request("https://example.com/"),
      new Response("body"),
      new Blob(["blob"]),
      new File(["file"], "name"),
      new Event("type"),
      new AbortController(),
      new TextDecoder(),
      new URL("https://example.com/"),
    ];
    if (keep) kept.push(objects);
  }
  return kept;
})"_kj;

template <Properties properties, Wrappers wrappers>
void scavengeWrappers(benchmark::State& state) {
  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  flags.setJsgPropertyOnPrototypeTemplate(properties == Properties::ON_PROTOTYPE);
  TestFixture fixture({.featureFlags = flags.asReader()});
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto context = env.js.v8Context();
    auto script = jsg::check(v8::Script::Compile(context, jsg::v8Str(env.isolate, MAKE_WRAPPERS)));
    auto make = jsg::check(script->Run(context)).As<v8::Function>();
    v8::Local<v8::Value> args[] = {
      v8::Integer::New(env.isolate, static_cast<int>(state.range(0))),
      v8::Boolean::New(env.isolate, wrappers == Wrappers::LIVE),
    };
    auto minorGc = [&]() {
      env.isolate->RequestGarbageCollectionForTesting(v8::Isolate::kMinorGarbageCollection);
    };
    for (auto _: state) {
      state.PauseTiming();
      v8::HandleScope handleScope(env.isolate);
      // Leaves the previous iteration's wrappers out of the timed GC.
      minorGc();
      auto kept = jsg::check(make->Call(context, context->Global(), kj::size(args), args));
      state.ResumeTiming();
      minorGc();
      benchmark::DoNotOptimize(*kept);
    }
  });
  state.SetItemsProcessed(state.iterations() * state.range(0));
}

void ScavengeDeadInstancePropertyWrappers(benchmark::State& state) {
  scavengeWrappers<Properties::ON_INSTANCE, Wrappers::DEAD>(state);
}

void ScavengeLiveInstancePropertyWrappers(benchmark::State& state) {
  scavengeWrappers<Properties::ON_INSTANCE, Wrappers::LIVE>(state);
}

void ScavengeDeadPrototypePropertyWrappers(benchmark::State& state) {
  scavengeWrappers<Properties::ON_PROTOTYPE, Wrappers::DEAD>(state);
}

void ScavengeLivePrototypePropertyWrappers(benchmark::State& state) {
  scavengeWrappers<Properties::ON_PROTOTYPE, Wrappers::LIVE>(state);
}

WD_BENCHMARK(ScavengeDeadInstancePropertyWrappers)->Arg(100);
WD_BENCHMARK(ScavengeLiveInstancePropertyWrappers)->Arg(100);
WD_BENCHMARK(ScavengeDeadPrototypePropertyWrappers)->Arg(100);
WD_BENCHMARK(ScavengeLivePrototypePropertyWrappers)->Arg(100);

}  // namespace
}  // namespace workerd
