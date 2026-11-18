// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// What capnp-kj adds to a call, and what copying a message costs on its own.
//
// * "call": `TestEcho.bounce()` returns the bytes it was sent, between each pair of C++ and Rust
//   client and server, on one KJ event loop. C++ to C++ is the baseline; the other pairs add the
//   bridge, including its copy of params and results on each crossing.
// * "copy": the bridge's copy alone, of a message holding that many bytes.
// * "tail event": ways to copy a streaming tail event out of an RPC message for Rust to read:
//   canonicalize(), against a plain copy, against the bridge's copy, and against totalSize(), a
//   walk over the same event that copies nothing.

#include <workerd/io/worker-interface.capnp.h>
#include <workerd/rust/capnp-kj/ffi.h>
#include <workerd/rust/capnp-kj/tests/ffi.rs.h>
#include <workerd/tests/bench-tools.h>

#include <kj-rs/kj-rs.h>
#include <rust/capnp-kj/tests/test.capnp.h>

#include <capnp/capability.h>
#include <capnp/message.h>
#include <kj/async.h>

namespace workerd::rust::capnp_kj::test {
namespace {

// Calls are made in batches so that entering Rust from C++ once per batch doesn't count as part
// of a call.
constexpr uint32_t CALLS_PER_ITERATION = 64;

const auto SIZES = {0, 1024, 16 * 1024, 256 * 1024};

class CxxBounce final: public TestEcho::Server {
 public:
  kj::Promise<void> bounce(BounceContext context) override {
    context.getResults().setData(context.getParams().getData());
    return kj::READY_NOW;
  }
};

enum class Side { CXX, RUST };

TestEcho::Client newServer(Side side) {
  if (side == Side::CXX) {
    return kj::heap<CxxBounce>();
  }
  return capnp_kj::clientFromRust<TestEcho>(new_echo_server(""));
}

kj::Promise<void> cxxBounceN(
    TestEcho::Client client, kj::ArrayPtr<const kj::byte> data, uint32_t count) {
  for (auto i KJ_UNUSED: kj::zeroTo(count)) {
    auto request = client.bounceRequest();
    request.setData(data);
    auto response = co_await request.send();
    KJ_ASSERT(response.getData().size() == data.size());
  }
}

// Calls sampled for allocation counts. Fewer than CALLS_PER_ITERATION, because sampling every
// allocation is slow; entering Rust once per batch is still spread over several calls.
constexpr uint32_t CALLS_PER_ALLOCATION_SAMPLE = 8;

void benchCall(benchmark::State& state, Side client, Side server) {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);
  auto size = static_cast<size_t>(state.range(0));
  auto data = kj::heapArray<kj::byte>(size);
  data.asPtr().fill(0x5a);
  auto target = newServer(server);

  auto batch = [&](uint32_t count) {
    if (client == Side::CXX) {
      cxxBounceN(target, data, count).wait(waitScope);
    } else {
      rust_bounce_n(capnp_kj::clientToRust(target), size, count).wait(waitScope);
    }
  };
  for (auto _: state) {
    batch(CALLS_PER_ITERATION);
  }
  state.counters["per_call"] = benchmark::Counter(CALLS_PER_ITERATION,
      benchmark::Counter::kIsIterationInvariantRate | benchmark::Counter::kInvert);

  // Allocations per call, only for empty payloads, where the bridge's fixed costs show.
  if (size == 0) {
    workerd::bench::recordAllocationStats(state, [&]() { batch(CALLS_PER_ALLOCATION_SAMPLE); });
    for (auto name: {"native_allocs/op", "native_bytes/op"}) {
      auto it = state.counters.find(name);
      if (it != state.counters.end()) {
        it->second.value /= CALLS_PER_ALLOCATION_SAMPLE;
      }
    }
  }
}

void benchCopy(benchmark::State& state) {
  auto size = static_cast<size_t>(state.range(0));
  capnp::MallocMessageBuilder source;
  auto data = kj::heapArray<kj::byte>(size);
  data.asPtr().fill(0x5a);
  source.initRoot<TestEcho::BounceParams>().setData(data);
  auto root = source.getRoot<capnp::AnyPointer>().asReader();

  for (auto _: state) {
    CxxMessage copy(root);
    benchmark::DoNotOptimize(copy.getSegments().begin());
  }
}

void addSizes(benchmark::internal::Benchmark* bench) {
  for (auto size: SIZES) {
    bench->Arg(size);
  }
  bench->Unit(benchmark::kNanosecond);
}

BENCHMARK_CAPTURE(benchCall, cxx_to_cxx, Side::CXX, Side::CXX)->Apply(addSizes);
BENCHMARK_CAPTURE(benchCall, cxx_to_rust, Side::CXX, Side::RUST)->Apply(addSizes);
BENCHMARK_CAPTURE(benchCall, rust_to_cxx, Side::RUST, Side::CXX)->Apply(addSizes);
BENCHMARK_CAPTURE(benchCall, rust_to_rust, Side::RUST, Side::RUST)->Apply(addSizes);
BENCHMARK(benchCopy)->Apply(addSizes);

// The same copy into a message with MallocMessageBuilder's default first segment, 1024 zeroed
// words, as CxxMessage made it before sizing its first segment.
void benchDefaultCopy(benchmark::State& state) {
  auto size = static_cast<size_t>(state.range(0));
  capnp::MallocMessageBuilder source;
  auto data = kj::heapArray<kj::byte>(size);
  data.asPtr().fill(0x5a);
  source.initRoot<TestEcho::BounceParams>().setData(data);
  auto root = source.getRoot<capnp::AnyPointer>().asReader();

  for (auto _: state) {
    capnp::MallocMessageBuilder copy;
    copy.setRoot(root);
    benchmark::DoNotOptimize(copy.getSegmentsForOutput().begin());
  }
}

BENCHMARK(benchDefaultCopy)->Apply(addSizes);

// Tail events like the ones a streaming tail worker receives: a log line, and the onset of a
// fetch with typical headers.

void buildLogEvent(rpc::Trace::TailEvent::Builder event) {
  event.setTimestampNs(1'700'000'000'000'000'000);
  event.setSequence(7);
  auto log = event.initEvent().initLog();
  log.setTimestampNs(1'700'000'000'000'000'000);
  log.setLogLevel(rpc::Trace::Log::Level::INFO);
  log.setMessage(
      "[\"handled request\",{\"path\":\"/api/items/42\",\"status\":200,\"durationMs\":12}]");
}

void buildFetchOnsetEvent(rpc::Trace::TailEvent::Builder event) {
  event.setTimestampNs(1'700'000'000'000'000'000);
  auto onset = event.initEvent().initOnset();
  onset.setScriptName("my-worker");
  onset.setEntryPoint("default");
  auto fetch = onset.initInfo().initFetch();
  fetch.setMethod(capnp::HttpMethod::GET);
  fetch.setUrl("https://example.com/api/items/42?include=details");
  fetch.setCfJson("{\"colo\":\"LHR\",\"country\":\"GB\",\"asn\":13335,\"httpProtocol\":\"HTTP/2\","
                  "\"tlsVersion\":\"TLSv1.3\",\"city\":\"London\",\"timezone\":\"Europe/London\"}");
  kj::StringPtr headers[][2] = {
    {"accept", "application/json"},
    {"accept-encoding", "gzip, br"},
    {"host", "example.com"},
    {"user-agent", "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36"},
    {"cf-connecting-ip", "203.0.113.7"},
    {"x-request-id", "4f6c2a9e-1b7d-4c3e-9a8f-2d5e6b7c8a9b"},
  };
  auto list = fetch.initHeaders(kj::size(headers));
  for (auto i: kj::indices(headers)) {
    list[i].setName(headers[i][0]);
    list[i].setValue(headers[i][1]);
  }
}

using BuildEvent = void (*)(rpc::Trace::TailEvent::Builder);

void benchCanonicalize(benchmark::State& state, BuildEvent build) {
  capnp::MallocMessageBuilder message;
  build(message.initRoot<rpc::Trace::TailEvent>());
  auto event = message.getRoot<rpc::Trace::TailEvent>().asReader();
  for (auto _: state) {
    auto words = capnp::canonicalize(event);
    benchmark::DoNotOptimize(words.begin());
  }
  state.counters["bytes"] = event.totalSize().wordCount * sizeof(capnp::word);
}

// A plain copy into a message sized to fit, the alternative to canonicalize() when Rust only
// needs to read the event and can take several segments.
void benchCopyEvent(benchmark::State& state, BuildEvent build) {
  capnp::MallocMessageBuilder message;
  build(message.initRoot<rpc::Trace::TailEvent>());
  auto event = message.getRoot<rpc::Trace::TailEvent>().asReader();
  auto words = event.totalSize().wordCount + 1;
  for (auto _: state) {
    capnp::MallocMessageBuilder copy(words);
    copy.setRoot(event);
    benchmark::DoNotOptimize(copy.getSegmentsForOutput().begin());
  }
}

// The bridge's own copy, CxxMessage, which walks the event to size its first segment.
void benchCxxMessageEvent(benchmark::State& state, BuildEvent build) {
  capnp::MallocMessageBuilder message;
  build(message.initRoot<rpc::Trace::TailEvent>());
  auto root = message.getRoot<capnp::AnyPointer>().asReader();
  for (auto _: state) {
    CxxMessage copy(root);
    benchmark::DoNotOptimize(copy.getSegments().begin());
  }
}

void benchWalk(benchmark::State& state, BuildEvent build) {
  capnp::MallocMessageBuilder message;
  build(message.initRoot<rpc::Trace::TailEvent>());
  auto event = message.getRoot<rpc::Trace::TailEvent>().asReader();
  for (auto _: state) {
    benchmark::DoNotOptimize(event.totalSize().wordCount);
  }
}

BENCHMARK_CAPTURE(benchCanonicalize, log, buildLogEvent)->Unit(benchmark::kNanosecond);
BENCHMARK_CAPTURE(benchCopyEvent, log, buildLogEvent)->Unit(benchmark::kNanosecond);
BENCHMARK_CAPTURE(benchCxxMessageEvent, log, buildLogEvent)->Unit(benchmark::kNanosecond);
BENCHMARK_CAPTURE(benchWalk, log, buildLogEvent)->Unit(benchmark::kNanosecond);
BENCHMARK_CAPTURE(benchCanonicalize, fetch_onset, buildFetchOnsetEvent)
    ->Unit(benchmark::kNanosecond);
BENCHMARK_CAPTURE(benchCopyEvent, fetch_onset, buildFetchOnsetEvent)->Unit(benchmark::kNanosecond);
BENCHMARK_CAPTURE(benchCxxMessageEvent, fetch_onset, buildFetchOnsetEvent)
    ->Unit(benchmark::kNanosecond);
BENCHMARK_CAPTURE(benchWalk, fetch_onset, buildFetchOnsetEvent)->Unit(benchmark::kNanosecond);

}  // namespace
}  // namespace workerd::rust::capnp_kj::test
