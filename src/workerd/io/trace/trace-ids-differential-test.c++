// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Checks that the Rust trace crate writes and reads ids as trace.c++ does, over generated and
// mutated inputs, so that the two implementations stay in step.

#include <workerd/io/trace.h>
#include <workerd/io/trace/test_bridge.rs.h>

#include <capnp/any.h>
#include <capnp/message.h>
#include <capnp/serialize.h>
#include <kj/encoding.h>
#include <kj/test.h>

namespace workerd::tracing {
namespace {

namespace ffi = rust::trace_test;

::rust::Slice<const uint8_t> bytesOf(kj::ArrayPtr<const char> s) {
  return {reinterpret_cast<const uint8_t*>(s.begin()), s.size()};
}

::rust::Slice<uint8_t> bytesOf(kj::ArrayPtr<char> s) {
  return {reinterpret_cast<uint8_t*>(s.begin()), s.size()};
}

// The Rust side of each C++ function under test, in its C++ types.
namespace rs {

kj::String toGoString(const TraceId& id) {
  char text[32]{};
  auto size = ffi::trace_id_to_go(id.getHigh(), id.getLow(), bytesOf(kj::arrayPtr(text)));
  return kj::heapString(text, size);
}

kj::String toW3C(const TraceId& id) {
  auto text = kj::heapString(32);
  ffi::trace_id_to_w3c(id.getHigh(), id.getLow(), bytesOf(text.asArray()));
  return text;
}

kj::Array<byte> toProtobuf(const TraceId& id) {
  auto bytes = kj::heapArray<byte>(16);
  ffi::trace_id_to_protobuf(id.getHigh(), id.getLow(), {bytes.begin(), bytes.size()});
  return bytes;
}

kj::String toGoString(SpanId id) {
  auto text = kj::heapString(16);
  ffi::span_id_to_go(id, bytesOf(text.asArray()));
  return text;
}

kj::String formatW3CTraceparent(const TraceId& traceId, SpanId spanId, TraceFlags flags) {
  auto text = kj::heapString(55);
  ffi::format_traceparent_to(
      traceId.getHigh(), traceId.getLow(), spanId, flags, bytesOf(text.asArray()));
  return text;
}

kj::Maybe<TraceId> toMaybe(ffi::ParsedTraceId parsed) {
  if (!parsed.ok) return kj::none;
  return TraceId(parsed.id.low, parsed.id.high);
}

kj::Maybe<TraceId> fromGoString(kj::ArrayPtr<const char> s) {
  return toMaybe(ffi::trace_id_from_go(bytesOf(s)));
}

kj::Maybe<TraceId> fromProtobuf(kj::ArrayPtr<const byte> buf) {
  return toMaybe(ffi::trace_id_from_protobuf({buf.begin(), buf.size()}));
}

kj::Maybe<SpanContext> tryFromTraceparent(kj::StringPtr tp) {
  auto parsed = ffi::parse_traceparent(bytesOf(tp));
  if (!parsed.ok) return kj::none;
  return SpanContext(TraceId(parsed.trace_id.low, parsed.trace_id.high), SpanId(parsed.span_id),
      TraceFlags(parsed.flags));
}

kj::Maybe<SpanContext> spanLinkParseContext(
    kj::StringPtr traceId, kj::StringPtr spanId, TraceFlags flags) {
  auto trace = KJ_UNWRAP_OR_RETURN(toMaybe(ffi::trace_id_from_w3c(bytesOf(traceId))), kj::none);
  auto span = ffi::span_id_from_w3c(bytesOf(spanId));
  if (!span.ok) return kj::none;
  return SpanContext(trace, SpanId(span.id), flags);
}

}  // namespace rs

constexpr size_t ROUNDS = 20000;

// splitmix64, so that a failure reproduces.
class Random {
 public:
  uint64_t next() {
    uint64_t z = (state += 0x9e3779b97f4a7c15);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9;
    z = (z ^ (z >> 27)) * 0x94d049bb133111eb;
    return z ^ (z >> 31);
  }

  size_t below(size_t n) {
    return next() % n;
  }

  // An id with each half zero, small or full now and then, which the formats treat specially.
  uint64_t id() {
    switch (below(4)) {
      case 0:
        return 0;
      case 1:
        return next() & 0xff;
      default:
        return next();
    }
  }

 private:
  uint64_t state = 0x5eed;
};

// Hex digits of both cases, and the characters that sit next to them.
// The high bytes are separate literals because a \x escape takes every hex digit after it.
constexpr kj::StringPtr ALPHABET = "0123456789abcdefABCDEF-gG/:@` x\x7f"
                                   "\x80"
                                   "\xff"_kj;

kj::String randomText(Random& random, size_t size) {
  auto text = kj::heapString(size);
  for (auto& c: text.asArray()) {
    // Mostly hex digits, so that many inputs parse.
    c = random.below(32) == 0 ? ALPHABET[random.below(ALPHABET.size())]
                              : ALPHABET[random.below(22)];
  }
  return text;
}

KJ_TEST("trace and span ids are written as the C++ writes them") {
  Random random;
  for (size_t i = 0; i < ROUNDS; i++) {
    // Drawn one per statement, so that the inputs don't depend on the order in which arguments
    // are evaluated.
    auto low = random.id();
    auto high = random.id();
    TraceId traceId(low, high);
    SpanId spanId(random.id());
    TraceFlags flags(random.next());
    KJ_ASSERT(rs::toGoString(traceId) == traceId.toGoString(), traceId.getHigh(), traceId.getLow());
    KJ_ASSERT(rs::toW3C(traceId) == traceId.toW3C());
    KJ_ASSERT(rs::toProtobuf(traceId) == traceId.toProtobuf());
    KJ_ASSERT(rs::toGoString(spanId) == spanId.toGoString(), spanId.getId());
    KJ_ASSERT(rs::formatW3CTraceparent(traceId, spanId, flags) ==
        formatW3CTraceparent(traceId, spanId, flags));
  }
}

// Returns whether the input parsed, so that a test can check it tried both outcomes.
template <typename T>
bool expectSame(kj::Maybe<T> actual, kj::Maybe<T> expected, kj::StringPtr input) {
  KJ_IF_SOME(e, expected) {
    auto& a = KJ_ASSERT_NONNULL(actual, input);
    KJ_ASSERT(a == e, input);
    return true;
  }
  KJ_ASSERT(actual == kj::none, input);
  return false;
}

bool expectSameContext(
    kj::Maybe<SpanContext> actual, kj::Maybe<SpanContext> expected, kj::StringPtr input) {
  KJ_IF_SOME(e, expected) {
    auto& a = KJ_ASSERT_NONNULL(actual, input);
    KJ_ASSERT(a == e, input);
    KJ_ASSERT(a.getTraceFlags() == e.getTraceFlags(), input);
    return true;
  }
  KJ_ASSERT(actual == kj::none, input);
  return false;
}

// Both outcomes are common enough for the comparison to mean something.
void expectMixed(size_t parsed) {
  KJ_ASSERT(parsed > ROUNDS / 10 && parsed < ROUNDS - ROUNDS / 10, parsed);
}

KJ_TEST("Go strings parse as the C++ parses them") {
  Random random;
  size_t parsed = 0;
  for (size_t i = 0; i < ROUNDS; i++) {
    auto text = randomText(random, random.below(35));
    if (text.size() > 0 && random.below(16) == 0) {
      text.asArray()[random.below(text.size())] = '\0';
    }
    parsed += expectSame(rs::fromGoString(text), TraceId::fromGoString(text), text);
  }
  expectMixed(parsed);
}

KJ_TEST("protobuf bytes parse as the C++ parses them") {
  Random random;
  size_t parsed = 0;
  for (size_t i = 0; i < ROUNDS; i++) {
    auto bytes = kj::heapArray<byte>(random.below(4) == 0 ? random.below(18) : 16);
    for (auto& b: bytes) b = random.next();
    auto input = kj::encodeHex(bytes);
    parsed += expectSame(rs::fromProtobuf(bytes), TraceId::fromProtobuf(bytes), input);
  }
  expectMixed(parsed);
}

KJ_TEST("traceparent headers parse as the C++ parses them") {
  Random random;
  size_t parsed = 0;
  for (size_t i = 0; i < ROUNDS; i++) {
    auto low = random.id();
    auto high = random.id();
    TraceId traceId(low, high);
    auto text = formatW3CTraceparent(traceId, SpanId(random.id()), TraceFlags(random.next()));
    // Change a few characters, or the length, or nothing.
    switch (random.below(4)) {
      case 0:
        break;
      case 1:
        text = kj::str(text.first(random.below(text.size())));
        break;
      case 2:
        text = kj::str(text, ALPHABET[random.below(ALPHABET.size())]);
        break;
      default:
        for (auto n = random.below(3) + 1; n > 0; n--) {
          text.asArray()[random.below(text.size())] = ALPHABET[random.below(ALPHABET.size())];
        }
    }
    parsed += expectSameContext(
        rs::tryFromTraceparent(text), SpanContext::tryFromTraceparent(text), text);
  }
  expectMixed(parsed);
}

KJ_TEST("span link ids parse as the C++ parses them") {
  Random random;
  size_t parsed = 0;
  for (size_t i = 0; i < ROUNDS; i++) {
    auto traceId = randomText(random, random.below(8) == 0 ? random.below(34) : 32);
    auto spanId = randomText(random, random.below(8) == 0 ? random.below(18) : 16);
    TraceFlags flags(random.next());
    auto input = kj::str(traceId, " ", spanId);
    parsed += expectSameContext(rs::spanLinkParseContext(traceId, spanId, flags),
        SpanLink::parseContext(traceId, spanId, flags), input);
  }
  expectMixed(parsed);
}

KJ_TEST("traceparent headers are written as the C++ writes them") {
  Random random;
  size_t written = 0;
  for (size_t i = 0; i < ROUNDS; i++) {
    auto low = random.id();
    auto high = random.id();
    auto spanId = random.id();
    auto flags = static_cast<uint8_t>(random.next());
    bool hasSpanId = random.below(4) != 0;
    bool hasFlags = random.below(4) != 0;
    SpanContext context(TraceId(low, high), hasSpanId ? kj::Maybe(SpanId(spanId)) : kj::none,
        hasFlags ? kj::Maybe(TraceFlags(flags)) : kj::none);
    char text[55]{};
    auto size = ffi::span_context_to_traceparent(
        high, low, spanId, hasSpanId, flags, hasFlags, bytesOf(kj::arrayPtr(text)));
    KJ_IF_SOME(expected, context.toTraceparent()) {
      KJ_ASSERT(kj::heapString(text, size) == expected, size);
      written++;
    } else {
      KJ_ASSERT(size == 0, low, high, spanId, hasSpanId, hasFlags);
    }
  }
  expectMixed(written);
}

// Overwrites some bytes of the struct's data section (union discriminants, enum codes, ids,
// flags) and clears some of its pointers, and does the same to the structs it points to.
void scramble(capnp::AnyStruct::Builder builder, Random& random) {
  for (auto& b: builder.getDataSection()) {
    if (random.below(8) == 0) b = random.next();
  }
  auto pointers = builder.getPointerSection();
  for (auto i: kj::indices(pointers)) {
    auto pointer = pointers[i];
    if (random.below(6) == 0) {
      pointer.clear();
    } else if (pointer.getPointerType() == capnp::PointerType::STRUCT) {
      scramble(pointer.getAs<capnp::AnyStruct>(), random);
    }
  }
}

void randomTraceId(rpc::TraceId::Builder builder, Random& random) {
  builder.setLow(random.id());
  builder.setHigh(random.id());
}

void randomTraceFlags(rpc::TraceFlags::Builder builder, Random& random) {
  if (random.below(2) == 0) {
    builder.getValue().setSet(random.next());
  } else {
    builder.getValue().setUnset();
  }
}

template <typename T>
kj::Array<capnp::word> canonical(capnp::MessageBuilder& message) {
  return capnp::canonicalize(message.getRoot<T>().asReader());
}

template <typename T>
kj::Array<capnp::word> canonical(kj::ArrayPtr<const uint8_t> flat) {
  auto words = kj::heapArray<capnp::word>(flat.size() / sizeof(capnp::word));
  memcpy(words.begin(), flat.begin(), words.asBytes().size());
  capnp::FlatArrayMessageReader reader(words);
  return capnp::canonicalize(reader.getRoot<T>());
}

// Builds random messages of type T, scrambles half of them, and checks that Rust reads and
// rewrites each as C++ does. `readWrite` is the C++ side: it reads the root and writes the value
// it read into `out`, or returns false if C++ rejects the message. Returns how many Rust accepted.
template <typename T, typename Fill, typename ReadWrite, typename RoundTrip>
size_t expectSameCapnp(Fill fill, ReadWrite readWrite, RoundTrip roundTrip) {
  Random random;
  size_t accepted = 0;
  for (size_t i = 0; i < ROUNDS; i++) {
    capnp::MallocMessageBuilder input;
    fill(input.initRoot<T>(), random);
    if (random.below(2) == 0) scramble(input.getRoot<capnp::AnyStruct>(), random);
    auto flat = capnp::messageToFlatArray(input);
    auto description = kj::encodeHex(flat.asBytes());

    auto rust =
        roundTrip(::rust::Slice<const uint8_t>(flat.asBytes().begin(), flat.asBytes().size()));
    capnp::MallocMessageBuilder output;
    bool cppAccepted = readWrite(input.getRoot<T>().asReader(), output);
    KJ_ASSERT(rust.ok == cppAccepted, description);
    if (rust.ok) {
      auto bytes = kj::arrayPtr(rust.message.data(), rust.message.size());
      KJ_ASSERT(canonical<T>(bytes).asBytes() == canonical<T>(output).asBytes(), description);
      accepted++;
    }
  }
  return accepted;
}

KJ_TEST("SpanContext capnp is read and written as the C++ does") {
  auto accepted =
      expectSameCapnp<rpc::SpanContext>([](rpc::SpanContext::Builder builder, Random& random) {
    if (random.below(8) != 0) randomTraceId(builder.initTraceId(), random);
    if (random.below(2) == 0) {
      builder.getInfo().setSpanId(random.id());
    } else {
      builder.getInfo().setEmpty();
    }
    if (random.below(3) != 0) randomTraceFlags(builder.initTraceFlags(), random);
  }, [](rpc::SpanContext::Reader reader, capnp::MessageBuilder& out) {
    SpanContext::fromCapnp(reader).toCapnp(out.initRoot<rpc::SpanContext>());
    return true;
  }, ffi::span_context_round_trip);
  KJ_ASSERT(accepted == ROUNDS);
}

KJ_TEST("InvocationSpanContext capnp is read and written as the C++ does") {
  auto accepted = expectSameCapnp<rpc::InvocationSpanContext>(
      [](rpc::InvocationSpanContext::Builder builder, Random& random) {
    if (random.below(8) != 0) randomTraceId(builder.initTraceId(), random);
    if (random.below(8) != 0) randomTraceId(builder.initInvocationId(), random);
    builder.setSpanId(random.id());
    if (random.below(3) != 0) randomTraceFlags(builder.initTraceFlags(), random);
  }, [](rpc::InvocationSpanContext::Reader reader, capnp::MessageBuilder& out) {
    KJ_IF_SOME(context, InvocationSpanContext::fromCapnp(reader)) {
      context.toCapnp(out.initRoot<rpc::InvocationSpanContext>());
      return true;
    }
    return false;
  }, ffi::invocation_span_context_round_trip);
  expectMixed(accepted);
}

KJ_TEST("SpanStatus capnp is read and written as the C++ does") {
  auto accepted =
      expectSameCapnp<rpc::SpanStatus>([](rpc::SpanStatus::Builder builder, Random& random) {
    builder.setCode(static_cast<rpc::SpanStatusCode>(random.below(3)));
    if (random.below(2) == 0) {
      builder.getMessage().setText(randomText(random, random.below(20)));
    } else {
      builder.getMessage().setNone();
    }
  }, [](rpc::SpanStatus::Reader reader, capnp::MessageBuilder& out) {
    SpanStatus(reader).copyTo(out.initRoot<rpc::SpanStatus>());
    return true;
  }, ffi::span_status_round_trip);
  KJ_ASSERT(accepted == ROUNDS);
}

}  // namespace
}  // namespace workerd::tracing
