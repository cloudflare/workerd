// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Checks that the Rust trace crate writes and reads ids, span contexts, span status, tag values,
// attributes, span links and event infos as trace.c++ does, over generated and mutated inputs, so
// that the two implementations stay in step.

#include <workerd/io/trace.h>
#include <workerd/io/trace/test_bridge.rs.h>

#include <capnp/any.h>
#include <capnp/message.h>
#include <capnp/serialize.h>
#include <kj/encoding.h>
#include <kj/test.h>

#include <limits>

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
// Fewer for the capnp tests, whose rounds build and compare messages.
constexpr size_t CAPNP_ROUNDS = 5000;

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

  // A double that is now and then one the formats treat specially: a NaN, an infinity, a signed
  // zero or a subnormal.
  double float64() {
    static constexpr double SPECIAL[] = {std::numeric_limits<double>::quiet_NaN(),
      -std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
      -std::numeric_limits<double>::infinity(), 0.0, -0.0,
      std::numeric_limits<double>::denorm_min()};
    if (below(4) == 0) return SPECIAL[below(kj::size(SPECIAL))];
    return static_cast<double>(static_cast<int64_t>(next())) / 7;
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
void expectMixed(size_t parsed, size_t rounds = ROUNDS) {
  KJ_ASSERT(parsed > rounds / 10 && parsed < rounds - rounds / 10, parsed);
}

// Most messages are accepted, and some, made invalid by scrambling, are rejected.
void expectMostlyAccepted(size_t accepted) {
  KJ_ASSERT(accepted > CAPNP_ROUNDS / 2 && accepted < CAPNP_ROUNDS, accepted);
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
// flags), clears some of its pointers and points others at the wrong kind of object, and does
// the same to the structs it points to, alone or in lists.
void scramble(capnp::AnyStruct::Builder builder, Random& random) {
  for (auto& b: builder.getDataSection()) {
    if (random.below(8) == 0) b = random.next();
  }
  auto pointers = builder.getPointerSection();
  for (auto i: kj::indices(pointers)) {
    auto pointer = pointers[i];
    if (random.below(6) == 0) {
      pointer.clear();
    } else if (pointer.getPointerType() != capnp::PointerType::NULL_ && random.below(12) == 0) {
      // Text where a struct belongs, and a struct where a list or text does.
      if (pointer.getPointerType() == capnp::PointerType::STRUCT) {
        pointer.setAs<capnp::Text>("x");
      } else {
        pointer.initAsAnyStruct(1, 0);
      }
    } else if (pointer.getPointerType() == capnp::PointerType::STRUCT) {
      scramble(pointer.getAs<capnp::AnyStruct>(), random);
    } else if (pointer.getPointerType() == capnp::PointerType::LIST) {
      auto list = pointer.getAs<capnp::AnyList>();
      if (list.getElementSize() == capnp::ElementSize::INLINE_COMPOSITE) {
        for (auto element: list.as<capnp::List<capnp::AnyStruct>>()) scramble(element, random);
      }
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
// it read into `out`, or returns false if C++ rejects the message. `skip` picks out the messages
// on which the two are known to differ; one it throws on is not skipped, as both reject it.
// Returns how many messages Rust accepted and how many were skipped.
struct CapnpCounts {
  size_t accepted = 0;
  size_t skipped = 0;
};

template <typename T, typename Fill, typename ReadWrite, typename RoundTrip, typename Skip>
CapnpCounts expectSameCapnp(Fill fill, ReadWrite readWrite, RoundTrip roundTrip, Skip skip) {
  Random random;
  CapnpCounts counts;
  for (size_t i = 0; i < CAPNP_ROUNDS; i++) {
    capnp::MallocMessageBuilder input;
    fill(input.initRoot<T>(), random);
    if (random.below(2) == 0) scramble(input.getRoot<capnp::AnyStruct>(), random);
    bool skipped = false;
    (void)kj::runCatchingExceptions([&]() { skipped = skip(input.getRoot<T>().asReader()); });
    if (skipped) {
      counts.skipped++;
      continue;
    }
    auto flat = capnp::messageToFlatArray(input);
    auto description = kj::encodeHex(flat.asBytes());

    auto rust =
        roundTrip(::rust::Slice<const uint8_t>(flat.asBytes().begin(), flat.asBytes().size()));
    // C++ reads the same flat bytes as Rust, so that both check them the same way.
    capnp::FlatArrayMessageReader reader(flat);
    capnp::MallocMessageBuilder output;
    // C++ throws where Rust returns an error: an unknown tag value variant, an attribute without
    // values, or a pointer of the wrong kind.
    bool cppAccepted = false;
    auto exception =
        kj::runCatchingExceptions([&]() { cppAccepted = readWrite(reader.getRoot<T>(), output); });
    KJ_ASSERT(rust.ok == cppAccepted, description, exception);
    if (rust.ok) {
      auto bytes = kj::arrayPtr(rust.message.data(), rust.message.size());
      KJ_ASSERT(canonical<T>(bytes).asBytes() == canonical<T>(output).asBytes(), description);
      counts.accepted++;
    }
  }
  return counts;
}

template <typename T, typename Fill, typename ReadWrite, typename RoundTrip>
size_t expectSameCapnp(Fill fill, ReadWrite readWrite, RoundTrip roundTrip) {
  return expectSameCapnp<T>(fill, readWrite, roundTrip, [](T::Reader) { return false; }).accepted;
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
  expectMostlyAccepted(accepted);
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
  expectMixed(accepted, CAPNP_ROUNDS);
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
  expectMostlyAccepted(accepted);
}

void randomTagValue(rpc::TagValue::Builder builder, Random& random) {
  auto size = random.below(4);
  switch (random.below(7)) {
    case 0:
      builder.setString(randomText(random, random.below(10)));
      break;
    case 1:
      builder.setBool(random.below(2));
      break;
    case 2:
      builder.setInt64(random.next());
      break;
    case 3:
      builder.setFloat64(random.float64());
      break;
    case 4: {
      auto list = builder.initStringArray(size);
      for (auto element: list) {
        if (random.below(3) == 0) {
          element.setNone();
        } else {
          element.setValue(randomText(random, random.below(6)));
        }
      }
      break;
    }
    case 5: {
      auto list = builder.initBoolArray(size);
      for (auto element: list) {
        if (random.below(3) == 0) {
          element.setNone();
        } else {
          element.setValue(random.below(2));
        }
      }
      break;
    }
    default: {
      auto list = builder.initFloat64Array(size);
      for (auto element: list) {
        if (random.below(3) == 0) {
          element.setNone();
        } else {
          element.setValue(random.float64());
        }
      }
    }
  }
}

void randomAttribute(rpc::Trace::Attribute::Builder builder, Random& random) {
  builder.setName(randomText(random, random.below(8)));
  auto values = builder.initValue(random.below(4));
  for (auto value: values) randomTagValue(value, random);
}

KJ_TEST("TagValue capnp is read and written as the C++ does") {
  auto accepted = expectSameCapnp<rpc::TagValue>(
      randomTagValue, [](rpc::TagValue::Reader reader, capnp::MessageBuilder& out) {
    serializeTagValue(out.initRoot<rpc::TagValue>(), deserializeTagValue(reader));
    return true;
  }, ffi::tag_value_round_trip);
  expectMixed(accepted, CAPNP_ROUNDS);
}

KJ_TEST("Attribute capnp is read and written as the C++ does") {
  auto accepted = expectSameCapnp<rpc::Trace::Attribute>(
      randomAttribute, [](rpc::Trace::Attribute::Reader reader, capnp::MessageBuilder& out) {
    Attribute(reader).copyTo(out.initRoot<rpc::Trace::Attribute>());
    return true;
  }, ffi::attribute_round_trip);
  expectMixed(accepted, CAPNP_ROUNDS);
}

KJ_TEST("SpanLinks capnp is read and written as the C++ does") {
  auto accepted = expectSameCapnp<rpc::Trace::SpanLinks>(
      [](rpc::Trace::SpanLinks::Builder builder, Random& random) {
    auto links = builder.initLinks(random.below(3));
    for (auto link: links) {
      auto context = link.initContext();
      randomTraceId(context.initTraceId(), random);
      context.getInfo().setSpanId(random.id());
      if (random.below(2) == 0) randomTraceFlags(context.initTraceFlags(), random);
      auto attributes = link.initAttributes(random.below(3));
      for (auto attribute: attributes) randomAttribute(attribute, random);
      link.setDroppedAttributesCount(random.below(5));
    }
  }, [](rpc::Trace::SpanLinks::Reader reader, capnp::MessageBuilder& out) {
    SpanLinks(reader).copyTo(out.initRoot<rpc::Trace::SpanLinks>());
    return true;
  }, ffi::span_links_round_trip);
  expectMixed(accepted, CAPNP_ROUNDS);
}

// Reads an info as `T(reader)` and writes it back with `copyTo`, as C++ does for each type of
// event info.
template <typename Info, typename T>
auto readWriteInfo() {
  return [](T::Reader reader, capnp::MessageBuilder& out) {
    Info(reader).copyTo(out.initRoot<T>());
    return true;
  };
}

void randomFetchEventInfo(rpc::Trace::FetchEventInfo::Builder builder, Random& random) {
  // Some past `ban`: `other` and the methods the schema doesn't know, which C++ rejects.
  builder.setMethod(static_cast<capnp::HttpMethod>(random.below(32)));
  builder.setUrl(randomText(random, random.below(12)));
  if (random.below(2) == 0) builder.setCfJson(randomText(random, random.below(8)));
  auto headers = builder.initHeaders(random.below(4));
  for (auto header: headers) {
    header.setName(randomText(random, random.below(6)));
    header.setValue(randomText(random, random.below(6)));
  }
}

void randomJsRpcEventInfo(rpc::Trace::JsRpcEventInfo::Builder builder, Random& random) {
  builder.setMethodName(randomText(random, random.below(8)));
}

void randomScheduledEventInfo(rpc::Trace::ScheduledEventInfo::Builder builder, Random& random) {
  builder.setScheduledTime(random.float64());
  builder.setCron(randomText(random, random.below(8)));
}

// The largest alarm time, in milliseconds either side of 1970, that C++ holds as a `kj::Date`.
constexpr int64_t ALARM_LIMIT = std::numeric_limits<int64_t>::max() / 1'000'000;

void randomAlarmEventInfo(rpc::Trace::AlarmEventInfo::Builder builder, Random& random) {
  static constexpr int64_t EDGES[] = {ALARM_LIMIT, -ALARM_LIMIT, ALARM_LIMIT + 1, -ALARM_LIMIT - 1,
    std::numeric_limits<int64_t>::max(), std::numeric_limits<int64_t>::min()};
  if (random.below(8) == 0) {
    builder.setScheduledTimeMs(EDGES[random.below(kj::size(EDGES))]);
  } else {
    builder.setScheduledTimeMs(static_cast<int64_t>(random.next()) % 10'000'000'000'000);
  }
}

void randomOnHibernateEventInfo(rpc::Trace::OnHibernateEventInfo::Builder builder, Random& random) {
  builder.setReason(static_cast<rpc::Trace::OnHibernateEventInfo::Reason>(random.below(3)));
}

void randomQueueEventInfo(rpc::Trace::QueueEventInfo::Builder builder, Random& random) {
  builder.setQueueName(randomText(random, random.below(8)));
  builder.setBatchSize(random.next());
}

void randomEmailEventInfo(rpc::Trace::EmailEventInfo::Builder builder, Random& random) {
  builder.setMailFrom(randomText(random, random.below(8)));
  builder.setRcptTo(randomText(random, random.below(8)));
  builder.setRawSize(random.next());
}

void randomTraceEventInfo(rpc::Trace::TraceEventInfo::Builder builder, Random& random) {
  auto traces = builder.initTraces(random.below(4));
  for (auto item: traces) {
    if (random.below(4) != 0) item.setScriptName(randomText(random, random.below(8)));
  }
}

void randomHibernatableWebSocketEventInfo(
    rpc::Trace::HibernatableWebSocketEventInfo::Builder builder, Random& random) {
  auto type = builder.initType();
  switch (random.below(3)) {
    case 0:
      type.setMessage();
      break;
    case 1: {
      auto close = type.initClose();
      close.setCode(random.next());
      close.setWasClean(random.below(2));
      break;
    }
    default:
      type.setError();
  }
}

// An alarm time C++ cannot hold as a `kj::Date`: it overflows in nanoseconds, where Rust keeps the
// milliseconds.
bool alarmOverflows(rpc::Trace::AlarmEventInfo::Reader reader) {
  auto ms = reader.getScheduledTimeMs();
  return ms > ALARM_LIMIT || ms < -ALARM_LIMIT;
}

// A reason the schema doesn't know: C++ keeps its number, which the Rust enum cannot hold.
bool unknownReason(rpc::Trace::OnHibernateEventInfo::Reader reader) {
  return static_cast<uint16_t>(reader.getReason()) > 2;
}

KJ_TEST("FetchEventInfo capnp is read and written as the C++ does") {
  auto accepted = expectSameCapnp<rpc::Trace::FetchEventInfo>(randomFetchEventInfo,
      readWriteInfo<FetchEventInfo, rpc::Trace::FetchEventInfo>(),
      ffi::fetch_event_info_round_trip);
  expectMixed(accepted, CAPNP_ROUNDS);
}

KJ_TEST("JsRpcEventInfo capnp is read and written as the C++ does") {
  auto accepted = expectSameCapnp<rpc::Trace::JsRpcEventInfo>(randomJsRpcEventInfo,
      readWriteInfo<JsRpcEventInfo, rpc::Trace::JsRpcEventInfo>(),
      ffi::js_rpc_event_info_round_trip);
  expectMostlyAccepted(accepted);
}

KJ_TEST("ScheduledEventInfo capnp is read and written as the C++ does") {
  auto accepted = expectSameCapnp<rpc::Trace::ScheduledEventInfo>(randomScheduledEventInfo,
      readWriteInfo<ScheduledEventInfo, rpc::Trace::ScheduledEventInfo>(),
      ffi::scheduled_event_info_round_trip);
  expectMostlyAccepted(accepted);
}

KJ_TEST("AlarmEventInfo capnp is read and written as the C++ does") {
  auto counts = expectSameCapnp<rpc::Trace::AlarmEventInfo>(randomAlarmEventInfo,
      readWriteInfo<AlarmEventInfo, rpc::Trace::AlarmEventInfo>(), ffi::alarm_event_info_round_trip,
      alarmOverflows);
  // Without pointers, nothing is rejected.
  KJ_ASSERT(counts.skipped > 0 && counts.accepted + counts.skipped == CAPNP_ROUNDS, counts.accepted,
      counts.skipped);
}

KJ_TEST("OnHibernateEventInfo capnp is read and written as the C++ does") {
  auto counts = expectSameCapnp<rpc::Trace::OnHibernateEventInfo>(randomOnHibernateEventInfo,
      readWriteInfo<OnHibernateEventInfo, rpc::Trace::OnHibernateEventInfo>(),
      ffi::on_hibernate_event_info_round_trip, unknownReason);
  // Without pointers, nothing is rejected.
  KJ_ASSERT(counts.skipped > 0 && counts.accepted + counts.skipped == CAPNP_ROUNDS, counts.accepted,
      counts.skipped);
}

KJ_TEST("QueueEventInfo capnp is read and written as the C++ does") {
  auto accepted = expectSameCapnp<rpc::Trace::QueueEventInfo>(randomQueueEventInfo,
      readWriteInfo<QueueEventInfo, rpc::Trace::QueueEventInfo>(),
      ffi::queue_event_info_round_trip);
  expectMostlyAccepted(accepted);
}

KJ_TEST("EmailEventInfo capnp is read and written as the C++ does") {
  auto accepted = expectSameCapnp<rpc::Trace::EmailEventInfo>(randomEmailEventInfo,
      readWriteInfo<EmailEventInfo, rpc::Trace::EmailEventInfo>(),
      ffi::email_event_info_round_trip);
  expectMostlyAccepted(accepted);
}

KJ_TEST("TraceEventInfo capnp is read and written as the C++ does") {
  auto accepted = expectSameCapnp<rpc::Trace::TraceEventInfo>(randomTraceEventInfo,
      readWriteInfo<TraceEventInfo, rpc::Trace::TraceEventInfo>(),
      ffi::trace_event_info_round_trip);
  expectMostlyAccepted(accepted);
}

KJ_TEST("HibernatableWebSocketEventInfo capnp is read and written as the C++ does") {
  auto accepted = expectSameCapnp<rpc::Trace::HibernatableWebSocketEventInfo>(
      randomHibernatableWebSocketEventInfo,
      readWriteInfo<HibernatableWebSocketEventInfo, rpc::Trace::HibernatableWebSocketEventInfo>(),
      ffi::hibernatable_web_socket_event_info_round_trip);
  // Scrambling makes an unknown type now and then, which both reject.
  expectMostlyAccepted(accepted);
}

KJ_TEST("TracePreview capnp is read and written as the C++ does") {
  auto accepted = expectSameCapnp<rpc::Trace::TracePreviewInfo>(
      [](rpc::Trace::TracePreviewInfo::Builder builder, Random& random) {
    builder.setId(randomText(random, random.below(8)));
    builder.setSlug(randomText(random, random.below(8)));
    builder.setName(randomText(random, random.below(8)));
  }, readWriteInfo<TracePreview, rpc::Trace::TracePreviewInfo>(), ffi::trace_preview_round_trip);
  expectMostlyAccepted(accepted);
}

KJ_TEST("FetchResponseInfo capnp is read and written as the C++ does") {
  auto accepted = expectSameCapnp<rpc::Trace::FetchResponseInfo>(
      [](rpc::Trace::FetchResponseInfo::Builder builder, Random& random) {
    builder.setStatusCode(random.next());
  }, readWriteInfo<FetchResponseInfo, rpc::Trace::FetchResponseInfo>(),
      ffi::fetch_response_info_round_trip);
  KJ_ASSERT(accepted == CAPNP_ROUNDS);
}

void randomOnsetInfo(rpc::Trace::Onset::Info::Builder builder, Random& random) {
  switch (random.below(11)) {
    case 0:
      randomFetchEventInfo(builder.initFetch(), random);
      break;
    case 1:
      randomJsRpcEventInfo(builder.initJsRpc(), random);
      break;
    case 2:
      randomScheduledEventInfo(builder.initScheduled(), random);
      break;
    case 3:
      randomAlarmEventInfo(builder.initAlarm(), random);
      break;
    case 4:
      randomQueueEventInfo(builder.initQueue(), random);
      break;
    case 5:
      randomEmailEventInfo(builder.initEmail(), random);
      break;
    case 6:
      randomTraceEventInfo(builder.initTrace(), random);
      break;
    case 7:
      randomHibernatableWebSocketEventInfo(builder.initHibernatableWebSocket(), random);
      break;
    case 8:
      builder.initConnect();
      break;
    case 9:
      randomOnHibernateEventInfo(builder.initOnHibernate(), random);
      break;
    default:
      builder.initCustom();
  }
}

KJ_TEST("Onset info capnp is read and written as the C++ does") {
  // C++ reads and writes the info only as part of an onset, so the messages are onsets, and only
  // their infos are compared.
  auto counts =
      expectSameCapnp<rpc::Trace::Onset>([](rpc::Trace::Onset::Builder builder, Random& random) {
    randomOnsetInfo(builder.initInfo(), random);
  }, [](rpc::Trace::Onset::Reader reader, capnp::MessageBuilder& out) {
    capnp::MallocMessageBuilder written;
    Onset(reader).copyTo(written.initRoot<rpc::Trace::Onset>());
    out.initRoot<rpc::Trace::Onset>().setInfo(written.getRoot<rpc::Trace::Onset>().getInfo());
    return true;
  }, ffi::event_info_round_trip, [](rpc::Trace::Onset::Reader reader) {
    auto info = reader.getInfo();
    switch (info.which()) {
      case rpc::Trace::Onset::Info::ALARM:
        return alarmOverflows(info.getAlarm());
      case rpc::Trace::Onset::Info::ON_HIBERNATE:
        return unknownReason(info.getOnHibernate());
      default:
        return false;
    }
  });
  expectMostlyAccepted(counts.accepted);
  KJ_ASSERT(counts.skipped > 0);
}

}  // namespace
}  // namespace workerd::tracing
