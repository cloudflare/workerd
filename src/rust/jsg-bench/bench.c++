// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/io/compatibility-date.capnp.h>
#include <workerd/jsg/jsg-test.h>
#include <workerd/rust/jsg-bench/lib.rs.h>
#include <workerd/rust/jsg/ffi-inl.h>
#include <workerd/rust/jsg/lib.rs.h>
#include <workerd/tests/bench-tools.h>

#include <capnp/message.h>
#include <kj/time.h>

#include <cstdio>
#include <cstdlib>
#include <initializer_list>

#ifdef __APPLE__
#include <malloc/malloc.h>
#endif

namespace workerd::jsg::test {
namespace {

namespace rustBench = ::workerd::rust::jsg_bench;
namespace rustJsg = ::workerd::rust::jsg;

struct Pair {
  double a;
  double b;
  JSG_STRUCT(a, b);
};

struct Octet {
  double a;
  double b;
  double c;
  double d;
  double e;
  double f;
  double g;
  double h;
  JSG_STRUCT(a, b, c, d, e, f, g, h);
};

class Node final: public Object {
 public:
  explicit Node(uint32_t value): value(value) {}

  static Ref<Node> constructor(Lock& js, uint32_t value) {
    return js.alloc<Node>(value);
  }
  uint32_t getValue() {
    return value;
  }
  void setChild(Ref<Node> node) {
    child = kj::mv(node);
  }
  kj::Maybe<Ref<Node>> getChild() {
    return child.map([](Ref<Node>& node) { return node.addRef(); });
  }

  JSG_RESOURCE_TYPE(Node) {
    JSG_READONLY_PROTOTYPE_PROPERTY(value, getValue);
    JSG_READONLY_PROTOTYPE_PROPERTY(child, getChild);
    JSG_METHOD(setChild);
  }

 private:
  uint32_t value;
  kj::Maybe<Ref<Node>> child;

  void visitForGc(GcVisitor& visitor) {
    visitor.visit(child);
  }
};

class Target final: public Object {
 public:
  explicit Target(Ref<Node> child): child(kj::mv(child)) {}

  void noop() {}
  static void staticNoop() {}
  uint32_t add(uint32_t a) {
    return a + 1;
  }
  uint32_t addFour(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    return a + b + c + d;
  }
  double number(double a) {
    return a + 0.5;
  }
  bool boolean(bool a) {
    return !a;
  }
  v8::Local<v8::Value> raw(v8::Local<v8::Value> a) {
    return a;
  }
  uint32_t getValue() {
    return value;
  }
  void setValue(uint32_t a) {
    value = a;
  }
  uint32_t optional(jsg::Optional<uint32_t> a) {
    return a.orDefault(7);
  }

  double union_(kj::OneOf<double, kj::String> a) {
    KJ_SWITCH_ONEOF(a) {
      KJ_CASE_ONEOF(n, double) {
        return n;
      }
      KJ_CASE_ONEOF(s, kj::String) {
        return s.size();
      }
    }
    KJ_UNREACHABLE;
  }

  uint32_t stringLen(kj::String a) {
    return static_cast<uint32_t>(a.size());
  }
  kj::String stringEcho(kj::String a) {
    return a;
  }
  double arraySum(kj::Array<double> a) {
    double sum = 0;
    for (auto n: a) sum += n;
    return sum;
  }
  kj::Array<double> arrayEcho(kj::Array<double> a) {
    return a;
  }
  kj::Array<double> arrayMake(uint32_t n) {
    auto result = kj::heapArray<double>(n);
    for (uint32_t i = 0; i < n; ++i) result[i] = i;
    return result;
  }
  double pairSum(Pair a) {
    return a.a + a.b;
  }
  Pair pairEcho(Pair a) {
    return a;
  }
  double octetSum(Octet a) {
    return a.a + a.b + a.c + a.d + a.e + a.f + a.g + a.h;
  }
  Octet octetMake() {
    return {1, 2, 3, 4, 5, 6, 7, 8};
  }
  uint32_t bytesLen(v8::Local<v8::Uint8Array> a) {
    return static_cast<uint32_t>(a->ByteLength());
  }
  uint32_t bytesCopy(v8::Local<v8::Uint8Array> a) {
    auto bytes = kj::heapArray(a->ByteLength() == 0
            ? kj::ArrayPtr<const kj::byte>()
            : kj::arrayPtr(static_cast<const kj::byte*>(a->Buffer()->Data()) + a->ByteOffset(),
                  a->ByteLength()));
    benchmark::DoNotOptimize(bytes.begin());
    return static_cast<uint32_t>(bytes.size());
  }
  uint32_t bytesSum(v8::Local<v8::Uint8Array> a) {
    if (a->ByteLength() == 0) return 0;
    auto bytes = kj::arrayPtr(
        static_cast<const kj::byte*>(a->Buffer()->Data()) + a->ByteOffset(), a->ByteLength());
    uint32_t sum = 0;
    for (auto n: bytes) sum += n;
    return sum;
  }
  Ref<Node> newNode(Lock& js, uint32_t n) {
    return js.alloc<Node>(n);
  }
  Ref<Node> sameNode() {
    return child.addRef();
  }
  uint32_t takeNode(Ref<Node> a) {
    return a->getValue();
  }
  double callback(Lock& js, v8::Local<v8::Function> a, double n) {
    v8::Local<v8::Value> args[] = {v8::Number::New(js.v8Isolate, n)};
    return check(check(a->Call(js.v8Context(), v8::Undefined(js.v8Isolate), 1, args))
                     ->NumberValue(js.v8Context()));
  }
  void fail() {
    JSG_FAIL_REQUIRE(TypeError, "benchmark error");
  }

  JSG_RESOURCE_TYPE(Target) {
    JSG_METHOD(noop);
    JSG_STATIC_METHOD(staticNoop);
    JSG_METHOD(add);
    JSG_METHOD(addFour);
    JSG_METHOD(number);
    JSG_METHOD(boolean);
    JSG_METHOD(raw);
    JSG_PROTOTYPE_PROPERTY(value, getValue, setValue);
    JSG_INSTANCE_PROPERTY(own, getValue, setValue);
    JSG_METHOD(optional);
    JSG_METHOD_NAMED(union, union_);
    JSG_METHOD(stringLen);
    JSG_METHOD(stringEcho);
    JSG_METHOD(arraySum);
    JSG_METHOD(arrayEcho);
    JSG_METHOD(arrayMake);
    JSG_METHOD(pairSum);
    JSG_METHOD(pairEcho);
    JSG_METHOD(octetSum);
    JSG_METHOD(octetMake);
    JSG_METHOD(bytesLen);
    JSG_METHOD(bytesCopy);
    JSG_METHOD(bytesSum);
    JSG_METHOD(newNode);
    JSG_METHOD(sameNode);
    JSG_METHOD(takeNode);
    JSG_METHOD(callback);
    JSG_METHOD(fail);
  }

 private:
  uint32_t value = 0;
  Ref<Node> child;
  void visitForGc(GcVisitor& visitor) {
    visitor.visit(child);
  }
};

class Context final: public Object, public ContextGlobal {
 public:
  Ref<Target> makeCpp(Lock& js) {
    return js.alloc<Target>(js.alloc<Node>(7));
  }
  JSG_RESOURCE_TYPE(Context) {
    JSG_NESTED_TYPE(Node);
    JSG_NESTED_TYPE(Target);
    JSG_METHOD(makeCpp);
  }
};

JSG_DECLARE_ISOLATE_TYPE(BenchIsolate, Context, Target, Node, Pair, Octet);
JSG_DECLARE_DEBUG_ISOLATE_TYPE(VerifyFastIsolate, Context, Target, Node, Pair, Octet);

V8System& system() {
  static V8System system({"--allow-natives-syntax"_kj, "--expose-gc"_kj});
  return system;
}

enum class Binding { CPP, RUST, CPP_FAST, RUST_FAST, CPP_STRUCT_FAST };
enum class CpuTime { THREAD, PROCESS };

struct Case {
  kj::String name;
  kj::String setup;
  kj::String expression;
  kj::String validation;
  kj::String expected;
  uint32_t batch;
};

// Verification runs count Fast API calls and abort at the first failed check, since Google
// Benchmark exits successfully when a case reports an error.
bool verifying() {
  return std::getenv("JSG_BENCH_VERIFY") != nullptr;
}

bool smokeTesting() {
  return std::getenv("JSG_BENCH_SMOKE") != nullptr;
}

void fail(benchmark::State& state, kj::StringPtr message) {
  if (verifying()) {
    KJ_LOG(FATAL, "benchmark check failed", state.name().c_str(), message);
    abort();
  }
  state.SkipWithError(message.cStr());
}

kj::StringPtr describe(const v8::String::Utf8Value& error) {
  return *error == nullptr ? "unknown JavaScript exception"_kj : kj::StringPtr(*error);
}

template <typename Isolate = BenchIsolate>
void runCase(benchmark::State& state, Binding binding, const Case& test) {
  auto rust = binding == Binding::RUST || binding == Binding::RUST_FAST;
  JsgConfig config = {
    .fastApiEnabled = binding == Binding::CPP_FAST || binding == Binding::RUST_FAST};
  // Initializes V8 before newIsolateGroup() needs it; argument evaluation order is unspecified.
  auto& v8System = system();
  Isolate isolate(v8System, newIsolateGroup(), config, kj::heap<IsolateObserver>());
  isolate.runInLockScope([&](auto& js) {
    if (binding == Binding::CPP_STRUCT_FAST) js.setUsingFastJsgStruct();
    capnp::MallocMessageBuilder message;
    auto flags = capnp::canonicalize(message.initRoot<CompatibilityFlags>().asReader());
    // The realm stays attached to the isolate until it is dropped.
    auto realm =
        rustJsg::realm_create(js.v8Isolate, {flags.asBytes().begin(), flags.asBytes().size()});
    auto context = js.template newContext<Context>();
    auto ctx = context.getHandle(js.v8Isolate);
    v8::Context::Scope scope(ctx);
    auto rustTarget =
        check(rustJsg::maybe_local_from_ffi<v8::Value>(rustBench::make_target(js.v8Isolate)));
    check(ctx->Global()->Set(ctx, js.str("rustTarget"_kj), rustTarget));
    auto rustNode =
        check(rustJsg::maybe_local_from_ffi<v8::Value>(rustBench::node_constructor(js.v8Isolate)));
    check(ctx->Global()->Set(ctx, js.str("RustNode"_kj), rustNode));
    auto prefix =
        rust ? "const t=rustTarget; const C=RustNode;"_kj : "const t=cppTarget; const C=Node;"_kj;
    auto naturalJit = std::getenv("JSG_BENCH_NATURAL_JIT") != nullptr;
    auto warmupBatch = smokeTesting() ? 1 : 64;
    auto optimize = !naturalJit
        ? kj::str("%PrepareFunctionForOptimization(batch); batch(", warmupBatch,
              "); %OptimizeFunctionOnNextCall(batch); batch(", warmupBatch, "); batch;")
        : kj::str("batch(", warmupBatch * 2, "); batch;");
    auto code = kj::str("const cppTarget=makeCpp();", prefix, test.setup, ";if (!(",
        test.validation, ")) throw new Error('validation: ", test.name,
        "');"
        "function batch(n) { let sum=0.5; for(let i=0;i<n;++i) { sum += (",
        test.expression,
        "); } return sum; }"
        "function expected(n) { let sum=0.5; for(let i=0;i<n;++i) { sum += (",
        test.expected, "); } return sum; }", optimize);
    v8::TryCatch catcher(js.v8Isolate);
    auto script = check(v8::Script::Compile(ctx, js.str(code)));
    v8::Local<v8::Value> compiled;
    if (!script->Run(ctx).ToLocal(&compiled)) {
      v8::String::Utf8Value error(js.v8Isolate, catcher.Exception());
      fail(state, describe(error));
      return;
    }
    auto function = compiled.As<v8::Function>();
    v8::Local<v8::Value> args[] = {v8::Integer::NewFromUnsigned(js.v8Isolate, test.batch)};
    auto invoke = [&]() {
      v8::HandleScope handles(js.v8Isolate);
      auto result = check(function->Call(ctx, ctx->Global(), 1, args));
      benchmark::DoNotOptimize(result);
    };
    if (!smokeTesting()) {
      auto start = kj::systemPreciseMonotonicClock().now();
      do {
        invoke();
      } while (kj::systemPreciseMonotonicClock().now() - start < 30 * kj::MILLISECONDS);
    }
    // Bit 5 is OptimizationStatus::kTurboFanned in V8's src/runtime/runtime.h.
    auto tierCheck = naturalJit ? ""_kj
                                : "if ((%GetOptimizationStatus(batch)&(1<<5))===0)"
                                  "throw new Error('batch is not TurboFan optimized');"_kj;
    auto verification = check(v8::Script::Compile(ctx,
        js.str(kj::str(tierCheck, "if(batch(", test.batch, ")!==expected(", test.batch,
            "))throw new Error('batch result: ", test.name, "');", tierCheck))));
    auto verify = [&]() {
      v8::Local<v8::Value> result;
      if (!verification->Run(ctx).ToLocal(&result)) {
        v8::String::Utf8Value error(js.v8Isolate, catcher.Exception());
        fail(state, describe(error));
        return false;
      }
      return true;
    };
    if (!verify()) return;
    if constexpr (kj::isSameType<Isolate, VerifyFastIsolate>()) callCounter.reset();
    // Rust JSG counts fast calls only in builds with debug assertions.
    kj::Maybe<uint64_t> rustFastCallsBefore;
    if (binding == Binding::RUST_FAST && verifying()) {
      rustFastCallsBefore = rustBench::fast_call_count();
      static bool warned = false;
      if (rustFastCallsBefore == kj::none && !warned) {
        warned = true;
        KJ_LOG(WARNING,
            "Rust fast calls are not counted; build with --config=rust-debug to check "
            "the RustFast cases");
      }
    }
    for (auto _: state) invoke();
    state.SetItemsProcessed(state.iterations() * test.batch);
    state.counters["ops/batch"] = test.batch;
    if constexpr (kj::isSameType<Isolate, VerifyFastIsolate>()) {
      state.counters["fast-calls"] = callCounter.fast;
      state.counters["slow-calls"] = callCounter.slow;
      if (callCounter.fast == 0) fail(state, "V8 did not use the Fast API callback");
      if (callCounter.slow != 0) fail(state, "V8 used a slow callback in a Fast API case");
    }
    KJ_IF_SOME(before, rustFastCallsBefore) {
      auto fastCalls = KJ_ASSERT_NONNULL(rustBench::fast_call_count()) - before;
      state.counters["fast-calls"] = fastCalls;
      if (fastCalls == 0) fail(state, "V8 did not use the Rust Fast API callback");
    }
    verify();
  });
}

void nativeLifetime(benchmark::State& state, Binding binding, uint32_t clones) {
  const uint32_t COUNT = smokeTesting() ? 4 : 1024;
  for (auto _: state) {
    if (binding == Binding::RUST) {
      benchmark::DoNotOptimize(rustBench::native_lifetime(COUNT, clones));
    } else {
      uint32_t result = 0;
      for (uint32_t i = 0; i < COUNT; ++i) {
        auto node = jsg::alloc<Node>(i);
        benchmark::DoNotOptimize(&node);
        for (uint32_t j = 0; j < clones; ++j) {
          auto copy = node.addRef();
          benchmark::DoNotOptimize(&copy);
        }
        result ^= node->getValue();
      }
      benchmark::DoNotOptimize(result);
    }
  }
  state.SetItemsProcessed(state.iterations() * COUNT);
  state.counters["ops/batch"] = COUNT;
}

int registerCases() {
  static kj::Vector<kj::Own<Case>> cases;
#ifdef __APPLE__
  for (auto binding: {Binding::CPP, Binding::RUST}) {
    auto name =
        binding == Binding::CPP ? "Metadata/native-memory-cpp" : "Metadata/native-memory-rust";
    benchmark::RegisterBenchmark(name, [binding](benchmark::State& state) {
      const uint32_t COUNT = smokeTesting() ? 4 : 100000;
      // Populate allocator caches before taking the live-allocation baseline.
      benchmark::DoNotOptimize(rustBench::native_lifetime(1024, 0));
      for (uint32_t i = 0; i < 1024; ++i) {
        auto node = jsg::alloc<Node>(i);
        benchmark::DoNotOptimize(&node);
      }
      malloc_statistics_t before;
      malloc_zone_statistics(nullptr, &before);
      auto record = [&](auto& owner) {
        malloc_statistics_t after;
        malloc_zone_statistics(nullptr, &after);
        state.counters["malloc-bytes/resource"] =
            static_cast<double>(after.size_in_use - before.size_in_use) / COUNT;
        state.counters["malloc-blocks/resource"] =
            static_cast<double>(after.blocks_in_use - before.blocks_in_use) / COUNT;
        state.counters["ops/batch"] = 1;
        for (auto _: state) benchmark::DoNotOptimize(&owner);
      };
      if (binding == Binding::RUST) {
        auto resources = rustBench::retain_native(COUNT);
        record(resources);
      } else {
        auto builder = kj::heapArrayBuilder<Ref<Node>>(COUNT);
        for (uint32_t i = 0; i < COUNT; ++i) builder.add(jsg::alloc<Node>(i));
        auto resources = builder.finish();
        record(resources);
      }
    });
  }
#endif
  auto add = [](kj::StringPtr name, kj::StringPtr setup, kj::StringPtr expression,
                 kj::StringPtr validation, kj::StringPtr expected, uint32_t batch = 1024,
                 std::initializer_list<Binding> bindings = {Binding::CPP, Binding::RUST},
                 CpuTime cpuTime = CpuTime::THREAD) {
    if (smokeTesting()) batch = 1;
    for (auto binding: bindings) {
      auto test = kj::heap<Case>(Case{kj::str(name), kj::str(setup), kj::str(expression),
        kj::str(validation), kj::str(expected), batch});
      auto* testPtr = test.get();
      cases.add(kj::mv(test));
      auto label = kj::str(binding == Binding::CPP ? "Cpp/"
              : binding == Binding::RUST           ? "Rust/"
              : binding == Binding::CPP_FAST       ? "CppFast/"
              : binding == Binding::RUST_FAST      ? "RustFast/"
                                                   : "CppStructFast/",
          name);
      auto* registered =
          benchmark::RegisterBenchmark(label.cStr(), [binding, testPtr](benchmark::State& state) {
        // Names the case before it runs, so that a crash in verification mode can be attributed.
        if (verifying()) std::fprintf(stderr, "Running %s\n", state.name().c_str());
        // Not every platform reports an uncaught exception.
        KJ_IF_SOME(exception, kj::runCatchingExceptions([&] {
          if (binding == Binding::CPP_FAST && verifying()) {
            runCase<VerifyFastIsolate>(state, binding, *testPtr);
          } else {
            runCase(state, binding, *testPtr);
          }
        })) {
          fail(state, kj::str(exception));
        }
      })->Unit(benchmark::kNanosecond);
      if (cpuTime == CpuTime::PROCESS) registered->MeasureProcessCPUTime();
    }
  };

  // Cases whose methods both bindings can call through V8's Fast API.
  const std::initializer_list<Binding> FAST = {
    Binding::CPP, Binding::RUST, Binding::CPP_FAST, Binding::RUST_FAST};
  add("baseline/js-loop", "", "i&255", "true", "i&255");
  add("dispatch/noop", "", "(t.noop(),1)", "t.noop()===undefined", "1", 1024, FAST);
  add("dispatch/static", "const S=t.constructor;", "(S.staticNoop(),1)",
      "S.staticNoop()===undefined", "1", 1024, FAST);
  add("dispatch/u32", "", "t.add(i&255)", "t.add(7)===8", "(i&255)+1", 1024, FAST);
  add("dispatch/u32x4", "", "t.addFour(i&255,2,3,4)", "t.addFour(1,2,3,4)===10", "(i&255)+9", 1024,
      FAST);
  add("dispatch/number", "", "t.number(i+0.25)", "t.number(1.25)===1.75", "i+0.75", 1024, FAST);
  add("dispatch/bool", "", "+t.boolean((i&1)!==0)", "t.boolean(true)===false", "1-(i&1)", 1024,
      FAST);
  add("dispatch/raw", "const x={};", "+(t.raw(x)===x)", "t.raw(x)===x", "1");
  add("property/proto-get", "t.value=7;", "t.value", "t.value===7", "7");
  add("property/proto-set", "", "(t.value=i&255,1)", "(t.value=7,t.value===7)", "1");
  add("property/instance-get", "t.own=7;", "t.own", "t.own===7", "7");
  add("property/instance-set", "", "(t.own=i&255,1)", "(t.own=7,t.own===7)", "1");
  add("optional/present", "", "t.optional(i&255)", "t.optional(3)===3", "i&255");
  add("optional/absent", "", "t.optional()", "t.optional()===7", "7");
  add("union/number", "", "t.union(i+0.25)", "t.union(1.25)===1.25", "i+0.25");
  add("union/string", "", "t.union('abc')", "t.union('abc')===3", "3");
  add("coercion/u32-string", "", "t.add('123')", "t.add('123')===124", "124");
  add("coercion/u32-object", "const x={valueOf(){return 123;}};", "t.add(x)", "t.add(x)===124",
      "124");
  add("struct/pair-in", "const x={a:1,b:2};", "t.pairSum(x)", "t.pairSum(x)===3", "3");
  add("struct/pair-roundtrip", "const x={a:1,b:2};", "t.pairEcho(x).b", "t.pairEcho(x).b===2", "2",
      1024, {Binding::CPP, Binding::RUST, Binding::CPP_STRUCT_FAST});
  add("struct/octet-in", "const x={a:1,b:2,c:3,d:4,e:5,f:6,g:7,h:8};", "t.octetSum(x)",
      "t.octetSum(x)===36", "36");
  add("struct/octet-out", "", "t.octetMake().h", "t.octetMake().h===8", "8", 1024,
      {Binding::CPP, Binding::RUST, Binding::CPP_STRUCT_FAST});
  add("resource/unwrap", "const x=t.sameNode();", "t.takeNode(x)", "t.takeNode(x)===7", "7");
  add("resource/cached-wrap", "const x=t.sameNode();", "+(t.sameNode()===x)", "t.sameNode()===x",
      "1");
  add("resource/new-wrap", "", "(t.newNode(i&255),1)", "t.newNode(7).value===7", "1", 256,
      {Binding::CPP, Binding::RUST}, CpuTime::PROCESS);
  add("resource/constructor", "", "(new C(i&255),1)", "new C(7).value===7", "1", 256,
      {Binding::CPP, Binding::RUST}, CpuTime::PROCESS);
  add("callback/reenter", "const f=x=>x+1;", "t.callback(f,i&255)", "t.callback(f,7)===8",
      "(i&255)+1", 256);
  add("error/native",
      "function attempt(){try{t.fail();}catch(e){return e instanceof TypeError ? 1 : 0;}}",
      "attempt()", "attempt()===1", "1", 64);
  add("error/conversion",
      "function attempt(){try{t.add(Symbol());}catch(e){return e instanceof TypeError ? 1 : 0;}}",
      "attempt()", "attempt()===1", "1", 64);
  add("gc/empty", "", "(gc(),1)", "true", "1", 1, {Binding::CPP, Binding::RUST}, CpuTime::PROCESS);
  for (auto size: smokeTesting() ? std::initializer_list<int>{10, 20}
                                 : std::initializer_list<int>{1000, 10000}) {
    add(kj::str("gc/allocate-collect/", size),
        kj::str("function cycle(){for(let j=0;j<", size, ";++j)t.newNode(j);gc();return 1;}"),
        "cycle()", "cycle()===1", "1", 1, {Binding::CPP, Binding::RUST}, CpuTime::PROCESS);
    add(kj::str("gc/live-wrappers/", size),
        kj::str("const live=Array.from({length:", size, "},(_,i)=>new C(i));"),
        "(gc(),live.length)", kj::str("live.length===", size), kj::str(size), 1,
        {Binding::CPP, Binding::RUST}, CpuTime::PROCESS);
  }
  auto chainSize = smokeTesting() ? 10 : 1000;
  add(kj::str("gc/live-chain/", chainSize),
      kj::str("let live=new C(0);for(let j=1;j<", chainSize,
          ";++j){const n=new C(j);n.setChild(live);live=n;}"),
      "(gc(),live.value)",
      kj::str("(()=>{gc();let n=live,k=", chainSize - 1,
          ";for(;n;n=n.child)if(n.value!==k--)return false;return k===-1;})()"),
      kj::str(chainSize - 1), 1, {Binding::CPP, Binding::RUST}, CpuTime::PROCESS);

  for (auto size: smokeTesting() ? std::initializer_list<int>{1, 16, 64}
                                 : std::initializer_list<int>{16, 1024, 65536}) {
    for (auto encoding: {"ascii"_kj, "latin1"_kj, "utf16"_kj}) {
      auto ch = encoding == "ascii" ? "a" : encoding == "latin1" ? "é" : "水";
      auto setup = kj::str("const x='", ch, "'.repeat(", size, ");");
      auto length = size * (encoding == "ascii" ? 1 : encoding == "latin1" ? 2 : 3);
      add(kj::str("string/in/", encoding, "/", size), setup, "t.stringLen(x)",
          kj::str("t.stringLen(x)===", length), kj::str(length), size >= 65536 ? 16 : 256);
      add(kj::str("string/roundtrip/", encoding, "/", size), setup, "t.stringEcho(x).length",
          "t.stringEcho(x)===x", kj::str(size), size >= 65536 ? 16 : 256);
    }
  }
  for (auto size: smokeTesting() ? std::initializer_list<int>{0, 1, 8}
                                 : std::initializer_list<int>{8, 64, 1024}) {
    auto setup = kj::str("const x=Array.from({length:", size, "},(_,i)=>i);");
    add(kj::str("array/in/", size), setup, "t.arraySum(x)",
        kj::str("t.arraySum(x)===", size * (size - 1) / 2), kj::str(size * (size - 1) / 2),
        size >= 1024 ? 16 : 128);
    add(kj::str("array/roundtrip/", size), setup, "t.arrayEcho(x).length",
        "t.arrayEcho(x).join(',')===x.join(',')", kj::str(size), size >= 1024 ? 16 : 128);
    add(kj::str("array/out/", size), "", kj::str("t.arrayMake(", size, ").length"),
        kj::str("(a=>a.length===", size, "&&a.every((v,i)=>v===i))(t.arrayMake(", size, "))"),
        kj::str(size), size >= 1024 ? 16 : 128);
  }
  for (auto size: smokeTesting() ? std::initializer_list<int>{0, 16, 32, 64}
                                 : std::initializer_list<int>{0, 16, 1024, 65536}) {
    auto setup = kj::str("const x=new Uint8Array(", size, ");x.fill(3);");
    if (size == 16 || size == (smokeTesting() ? 32 : 1024)) {
      add(kj::str("buffer/length/", size), setup, "t.bytesLen(x)",
          kj::str("t.bytesLen(x)===", size), kj::str(size));
    }
    add(kj::str("buffer/copy-in/", size), setup, "t.bytesCopy(x)",
        kj::str("t.bytesCopy(x)===", size), kj::str(size), size >= 65536 ? 16 : 256);
    add(kj::str("buffer/borrow-sum/", size), setup, "t.bytesSum(x)",
        kj::str("t.bytesSum(x)===", size * 3), kj::str(size * 3), size >= 65536 ? 16 : 256);
  }
  for (auto clones: {0u, 1u, 16u}) {
    for (auto binding: {Binding::CPP, Binding::RUST}) {
      auto name = kj::str(binding == Binding::CPP ? "Cpp/" : "Rust/", "native/lifetime/", clones);
      benchmark::RegisterBenchmark(name.cStr(), [binding, clones](benchmark::State& state) {
        nativeLifetime(state, binding, clones);
      })->Unit(benchmark::kNanosecond);
    }
  }
  return 0;
}

auto registered = registerCases();

}  // namespace
}  // namespace workerd::jsg::test
