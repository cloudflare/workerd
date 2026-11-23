// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Goldens of the OTLP requests OtlpSpanSubmitter encodes. An exporter forwards these bytes to its
// collector unchanged, so the goldens pin them byte for byte, attribute order included. The tests
// run in predictable mode: timestamps are zero and span ids count up.
//
// A golden that no longer matches is reported with the fields that differ, decoded with the
// generated OTLP classes, and with the literal that would replace it. To regenerate the goldens
// after an intended change, run
//
//   bazel test //src/workerd/io:otlp-span-submitter-test@ --test_output=errors
//
// and paste each printed literal over its golden.

#include <workerd/io/invocation-span.h>
#include <workerd/io/otlp-span-submitter.h>
#include <workerd/util/thread-scopes.h>

#include <google/protobuf/util/message_differencer.h>
#include <opentelemetry/proto/collector/logs/v1/logs_service.pb.h>
#include <opentelemetry/proto/collector/trace/v1/trace_service.pb.h>

#include <kj/encoding.h>
#include <kj/test.h>

namespace workerd {
namespace {

namespace otlp = ::opentelemetry::proto;
using TraceRequest = otlp::collector::trace::v1::ExportTraceServiceRequest;
using LogsRequest = otlp::collector::logs::v1::ExportLogsServiceRequest;

// Predictable mode makes span ids without entropy.
class NoEntropySource final: public kj::EntropySource {
 public:
  void generate(kj::ArrayPtr<kj::byte> buffer) override {
    KJ_FAIL_ASSERT("predictable mode should not draw entropy");
  }
};

class RecordingExporter final: public OtlpSpanExporter {
 public:
  kj::Vector<kj::Array<const kj::byte>> spanRequests;
  kj::Vector<uint> spanCounts;
  kj::Vector<kj::Array<const kj::byte>> logRequests;
  kj::Vector<uint> logCounts;
  uint dropped = 0;
  // The names the submitter asked about, and the one name refused for each kind of span.
  kj::Vector<kj::String> runtimeSpansAsked;
  kj::Vector<kj::String> userSpansAsked;
  kj::StringPtr refusedRuntimeSpan;
  kj::StringPtr refusedUserSpan;

  void exportSpans(kj::Array<const kj::byte> request, uint spanCount) override {
    spanRequests.add(kj::mv(request));
    spanCounts.add(spanCount);
  }
  void exportLogs(kj::Array<const kj::byte> request, uint recordCount) override {
    logRequests.add(kj::mv(request));
    logCounts.add(recordCount);
  }
  void spansDropped(uint count) override {
    dropped += count;
  }
  bool exportsRuntimeSpan(const kj::ConstString& operationName) override {
    runtimeSpansAsked.add(kj::str(operationName));
    return operationName.asPtr() != refusedRuntimeSpan;
  }
  bool exportsUserSpan(const kj::ConstString& operationName) override {
    userSpansAsked.add(kj::str(operationName));
    return operationName.asPtr() != refusedUserSpan;
  }
};

// `hex` as C++ string literal lines, ready to paste over a golden.
kj::String hexLiteral(kj::StringPtr hex) {
  constexpr size_t LINE = 88;
  kj::Vector<kj::String> lines;
  for (size_t i = 0; i < hex.size(); i += LINE) {
    lines.add(kj::str("    \"", hex.slice(i, kj::min(i + LINE, hex.size())), "\""));
  }
  return kj::str(kj::strArray(lines, "\n"), "_kj;");
}

// Decodes `request` as a `Message` and checks that its hex encoding is `golden`. On a mismatch it
// names the fields that differ and prints the literal that would make `name` match.
template <typename Message>
Message expectGolden(
    kj::StringPtr name, kj::ArrayPtr<const kj::byte> request, kj::StringPtr golden) {
  Message actual;
  KJ_ASSERT(actual.ParseFromArray(request.begin(), request.size()), name);
  auto hex = kj::encodeHex(request);
  if (hex != golden) {
    std::string differences = "the golden does not decode";
    auto expectedBytes = kj::decodeHex(golden);
    Message expected;
    if (!expectedBytes.hadErrors &&
        expected.ParseFromArray(expectedBytes.begin(), expectedBytes.size())) {
      differences.clear();
      google::protobuf::util::MessageDifferencer differencer;
      differencer.ReportDifferencesToString(&differences);
      if (differencer.Compare(expected, actual)) {
        differences = "the decoded requests are equal; only their encodings differ";
      }
    }
    KJ_FAIL_EXPECT("golden mismatch", name, differences.c_str(),
        kj::str("replace it with\n", name, " =\n", hexLiteral(hex)));
  }
  return actual;
}

TraceRequest expectTraceGolden(
    kj::StringPtr name, kj::ArrayPtr<const kj::byte> request, kj::StringPtr golden) {
  auto decoded = expectGolden<TraceRequest>(name, request, golden);
  KJ_EXPECT(decoded.resource_spans_size() == 1, name);
  KJ_EXPECT(decoded.resource_spans(0).scope_spans_size() == 1, name);
  return decoded;
}

LogsRequest expectLogsGolden(
    kj::StringPtr name, kj::ArrayPtr<const kj::byte> request, kj::StringPtr golden) {
  auto decoded = expectGolden<LogsRequest>(name, request, golden);
  KJ_EXPECT(decoded.resource_logs_size() == 1, name);
  KJ_EXPECT(decoded.resource_logs(0).scope_logs_size() == 1, name);
  return decoded;
}

const google::protobuf::RepeatedPtrField<otlp::logs::v1::LogRecord>& records(
    const LogsRequest& request) {
  return request.resource_logs(0).scope_logs(0).log_records();
}

const google::protobuf::RepeatedPtrField<otlp::trace::v1::Span>& spans(
    const TraceRequest& request) {
  return request.resource_spans(0).scope_spans(0).spans();
}

const otlp::common::v1::AnyValue& attribute(
    const google::protobuf::RepeatedPtrField<otlp::common::v1::KeyValue>& attributes,
    kj::StringPtr key) {
  for (auto& kv: attributes) {
    if (kv.key() == key.cStr()) return kv.value();
  }
  KJ_FAIL_ASSERT("missing attribute", key);
}

bool hasAttribute(const otlp::trace::v1::Span& span, kj::StringPtr key) {
  for (auto& kv: span.attributes()) {
    if (kv.key() == key.cStr()) return true;
  }
  return false;
}

std::string spanId(uint64_t id) {
  std::string bytes(8, '\0');
  for (int i = 7; i >= 0; --i, id >>= 8) bytes[i] = static_cast<char>(id & 0xff);
  return bytes;
}

Span::TagValue text(kj::StringPtr value) {
  return kj::ConstString(kj::str(value));
}

constexpr tracing::TraceId TRACE_ID(0x1112131415161718, 0x0102030405060708);
constexpr uint64_t CALLER_SPAN_ID = 0xca11e5;
constexpr uint64_t INVOCATION_SPAN_ID = 0x1d;

// An invocation's resource and the tags on each of its spans, of all four tag types, in the shape
// makeOtlpResource() and makeOtlpIdentityTags() give them.
kj::Array<Span::Tag> testResource() {
  return kj::arr(Span::Tag{"telemetry.sdk.name"_kjc, text("workers-runtime")},
      Span::Tag{"cloudflare.script_name"_kjc, text("my-worker")},
      Span::Tag{"cloudflare.test.int"_kjc, static_cast<int64_t>(-7)},
      Span::Tag{"cloudflare.test.double"_kjc, 1.5}, Span::Tag{"cloudflare.test.bool"_kjc, true});
}

kj::Array<Span::Tag> testDefaultTags() {
  return kj::arr(Span::Tag{"cloudflare.entrypoint"_kjc, text("MyEntrypoint")},
      Span::Tag{"cloudflare.durable_object.id"_kjc, text("0123abcd")});
}

tracing::FetchEventInfo testFetchEvent() {
  return tracing::FetchEventInfo(kj::HttpMethod::GET, kj::str("https://example.com/path?q=1"),
      kj::str(R"({"asn":13335,"country":"PT","city":"Lisbon","continent":"EU"})"),
      kj::arr(tracing::FetchEventInfo::Header(kj::str("user-agent"), kj::str("test/1.0")),
          tracing::FetchEventInfo::Header(kj::str("cookie"), kj::str("secret"))));
}

constexpr kj::StringPtr INVOCATION_TREE =
    "0ae8100ad2010a1b0a0c736572766963652e6e616d65120b0a096d792d776f726b65720a270a1274656c656d"
    "657472792e73646b2e6e616d6512110a0f776f726b6572732d72756e74696d650a250a16636c6f7564666c61"
    "72652e7363726970745f6e616d65120b0a096d792d776f726b65720a220a13636c6f7564666c6172652e7465"
    "73742e696e74120b18f9ffffffffffffffff010a230a16636c6f7564666c6172652e746573742e646f75626c"
    "65120921000000000000f83f0a1a0a14636c6f7564666c6172652e746573742e626f6f6c1202100112900f0a"
    "110a0f776f726b6572732d72756e74696d6512ab040a10010203040506070811121314151617181208000000"
    "00000000012208000000000000001d2a05666574636830034a380a0875726c2e66756c6c122c0a2a68747470"
    "733a2f2f6170692e6578616d706c652e636f6d3a383434332f76312f6974656d733f69643d374a1c0a136874"
    "74702e726571756573742e6d6574686f6412050a034745544a200a19687474702e726573706f6e73652e7374"
    "617475735f636f646512031894034a220a15636c6f7564666c6172652e746573742e726174696f1209210000"
    "00000000d03f4a1c0a16636c6f7564666c6172652e746573742e636163686564120210014a150a0a75726c2e"
    "736368656d6512070a0568747470734a230a0e7365727665722e6164647265737312110a0f6170692e657861"
    "6d706c652e636f6d4a120a0b7365727665722e706f7274120318fb414a170a0875726c2e70617468120b0a09"
    "2f76312f6974656d734a1f0a156e6574776f726b2e70726f746f636f6c2e6e616d6512060a04687474704a13"
    "0a0975726c2e717565727912060a0469643d374a270a15636c6f7564666c6172652e656e747279706f696e74"
    "120e0a0c4d79456e747279706f696e744a2a0a1c636c6f7564666c6172652e64757261626c655f6f626a6563"
    "742e6964120a0a0830313233616263644a2b0a25636c6f7564666c6172652e696e766f636174696f6e2e7365"
    "7175656e63652e6e756d626572120218024a130a0a6572726f722e7479706512050a033430347a0218028501"
    "0100000012d1010a100102030405060708111213141516171812080000000000000003220800000000000000"
    "022a1f64757261626c655f6f626a6563745f73746f726167655f736574416c61726d30044a270a15636c6f75"
    "64666c6172652e656e747279706f696e74120e0a0c4d79456e747279706f696e744a2a0a1c636c6f7564666c"
    "6172652e64757261626c655f6f626a6563742e6964120a0a0830313233616263644a2b0a25636c6f7564666c"
    "6172652e696e766f636174696f6e2e73657175656e63652e6e756d6265721202180485010100000012f8020a"
    "1001020304050607081112131415161718120800000000000000022208000000000000001d2a0c72656e616d"
    "65642d776f726b30034a0f0a096170702e6974656d73120218034a270a15636c6f7564666c6172652e656e74"
    "7279706f696e74120e0a0c4d79456e747279706f696e744a2a0a1c636c6f7564666c6172652e64757261626c"
    "655f6f626a6563742e6964120a0a0830313233616263644a2b0a25636c6f7564666c6172652e696e766f6361"
    "74696f6e2e73657175656e63652e6e756d626572120218035a98011209657863657074696f6e1a1d0a0e6578"
    "63657074696f6e2e74797065120b0a09547970654572726f721a200a11657863657074696f6e2e6d65737361"
    "6765120b0a0962616420696e7075741a4a0a14657863657074696f6e2e737461636b747261636512320a3054"
    "7970654572726f723a2062616420696e7075740a20202020617420776f726b2028776f726b65722e6a733a31"
    "3a31297a0c12086578706c69636974180285010100000012fd050a1001020304050607081112131415161718"
    "1208000000000000001d22080000000000ca11e52a0347455430024a290a1a636c6f7564666c6172652e6578"
    "65637574696f6e5f6d6f64656c120b0a0973746174656c6573734a1f0a19636c6f7564666c6172652e746573"
    "742e726f6f745f626f6f6c120210004a160a0c666161732e7472696767657212060a04687474704a220a1763"
    "6c6f7564666c6172652e68616e646c65725f7479706512070a0566657463684a1c0a13687474702e72657175"
    "6573742e6d6574686f6412050a034745544a2a0a0875726c2e66756c6c121e0a1c68747470733a2f2f657861"
    "6d706c652e636f6d2f706174683f713d314a210a13757365725f6167656e742e6f726967696e616c120a0a08"
    "746573742f312e304a150a0e636c6f7564666c6172652e61736e12031897684a180a1067656f2e636f756e74"
    "72792e636f646512040a0250544a1d0a1167656f2e6c6f63616c6974792e6e616d6512080a064c6973626f6e"
    "4a1a0a1267656f2e636f6e74696e656e742e636f646512040a0245554a200a19687474702e726573706f6e73"
    "652e7374617475735f636f6465120318c8014a1a0a12636c6f7564666c6172652e6f7574636f6d6512040a02"
    "6f6b4a110a0b6370755f74696d655f6d73120218004a120a0c77616c6c5f74696d655f6d73120218004a150a"
    "0a75726c2e736368656d6512070a0568747470734a1f0a0e7365727665722e61646472657373120d0a0b6578"
    "616d706c652e636f6d4a130a0875726c2e7061746812070a052f706174684a1f0a156e6574776f726b2e7072"
    "6f746f636f6c2e6e616d6512060a04687474704a120a0975726c2e717565727912050a03713d314a270a1563"
    "6c6f7564666c6172652e656e747279706f696e74120e0a0c4d79456e747279706f696e744a2a0a1c636c6f75"
    "64666c6172652e64757261626c655f6f626a6563742e6964120a0a0830313233616263644a2b0a25636c6f75"
    "64666c6172652e696e766f636174696f6e2e73657175656e63652e6e756d62657212021801850101000000"_kj;

KJ_TEST("an invocation's span tree is exported in one request when its root closes") {
  setPredictableModeForTest();
  NoEntropySource entropy;
  auto exporter = kj::rc<RecordingExporter>();
  auto resource = testResource();
  auto defaultTags = testDefaultTags();
  {
    auto started = startExportedInvocationSpan(entropy, exporter.addRef(),
        tracing::SpanContext(TRACE_ID, tracing::SpanId(CALLER_SPAN_ID), tracing::TraceFlags(1)),
        {
          .serviceName = "my-worker",
          .resource = resource,
          .defaultTags = defaultTags,
        },
        tracing::SpanId(INVOCATION_SPAN_ID));
    KJ_EXPECT(started.logs == kj::none);
    auto& root = started.span;
    // Tags such as an embedder sets on the invocation span.
    root.setTag("cloudflare.execution_model"_kjc, "stateless"_kj);
    root.setTag("cloudflare.test.root_bool"_kjc, false);
    describeInvocationSpan(root, testFetchEvent());
    {
      auto fetch = root.newChild("fetch"_kjc);
      fetch.setTag("url.full"_kjc, "https://api.example.com:8443/v1/items?id=7"_kj);
      fetch.setTag("http.request.method"_kjc, "GET"_kj);
      fetch.setTag("http.response.status_code"_kjc, static_cast<int64_t>(404));
      fetch.setTag("cloudflare.test.ratio"_kjc, 0.25);
      fetch.setTag("cloudflare.test.cached"_kjc, true);
      // Neither span logs nor links are exported.
      fetch.addLog(kj::UNIX_EPOCH, "log"_kjc, text("not exported"));
      fetch.addLinks(tracing::SpanLinks(kj::arr(tracing::SpanLink(
          tracing::SpanContext(tracing::TraceId(1, 2), tracing::SpanId(3)), nullptr))));
    }
    {
      auto user = SpanBuilder(
          KJ_ASSERT_NONNULL(root.getObserver()).newChildFromUserCode(), "user-work"_kjc);
      user.setOperationName("renamed-work"_kjc);
      user.setTag("app.items"_kjc, static_cast<int64_t>(3));
      user.recordException(kj::none, kj::str("TypeError"), kj::str("bad input"),
          kj::str("TypeError: bad input\n    at work (worker.js:1:1)"));
      user.setStatus(
          tracing::SpanStatus(tracing::SpanStatusCode::ERROR, kj::ConstString("explicit"_kjc)));
      { auto setAlarm = user.newChild("durable_object_storage_setAlarm"_kjc); }
    }
    setInvocationResponseStatus(root, 200);
    setInvocationOutcome(root, EventOutcome::OK, 3 * kj::MILLISECONDS, 5 * kj::MILLISECONDS);
    // Closed spans wait for the root.
    KJ_EXPECT(exporter->spanRequests.size() == 0);
  }
  KJ_ASSERT(exporter->spanRequests.size() == 1);
  KJ_EXPECT(exporter->spanCounts[0] == 4);
  KJ_EXPECT(exporter->dropped == 0);
  // The root is not asked about; the others are, under the name they were opened with.
  KJ_EXPECT(exporter->runtimeSpansAsked.size() == 2);
  KJ_EXPECT(exporter->runtimeSpansAsked[0] == "fetch");
  KJ_EXPECT(exporter->runtimeSpansAsked[1] == "durable_object_storage_setAlarm");
  KJ_EXPECT(exporter->userSpansAsked.size() == 1);
  KJ_EXPECT(exporter->userSpansAsked[0] == "user-work");

  auto request = expectTraceGolden("INVOCATION_TREE", exporter->spanRequests[0], INVOCATION_TREE);
  auto& resourceAttributes = request.resource_spans(0).resource().attributes();
  KJ_EXPECT(resourceAttributes.size() == 6);
  KJ_EXPECT(resourceAttributes[0].key() == "service.name");
  KJ_EXPECT(resourceAttributes[0].value().string_value() == "my-worker");
  KJ_EXPECT(request.resource_spans(0).scope_spans(0).scope().name() == "workers-runtime");
  auto& out = spans(request);
  KJ_ASSERT(out.size() == 4);
  // In close order.
  auto& fetch = out[0];
  auto& setAlarm = out[1];
  auto& user = out[2];
  auto& root = out[3];
  KJ_EXPECT(root.name() == "GET");
  KJ_EXPECT(root.span_id() == spanId(INVOCATION_SPAN_ID));
  KJ_EXPECT(root.parent_span_id() == spanId(CALLER_SPAN_ID));
  KJ_EXPECT(root.kind() == otlp::trace::v1::Span::SPAN_KIND_SERVER);
  KJ_EXPECT(root.flags() == 1);
  KJ_EXPECT(root.start_time_unix_nano() == 0);
  KJ_EXPECT(attribute(root.attributes(), "cloudflare.invocation.sequence.number").int_value() == 1);
  KJ_EXPECT(attribute(root.attributes(), "cloudflare.entrypoint").string_value() == "MyEntrypoint");
  KJ_EXPECT(attribute(root.attributes(), "url.query").string_value() == "q=1");
  KJ_EXPECT(!root.has_status());
  KJ_EXPECT(fetch.kind() == otlp::trace::v1::Span::SPAN_KIND_CLIENT);
  KJ_EXPECT(fetch.parent_span_id() == spanId(INVOCATION_SPAN_ID));
  KJ_EXPECT(attribute(fetch.attributes(), "server.port").int_value() == 8443);
  // A 4xx response makes a non-root span an error.
  KJ_EXPECT(attribute(fetch.attributes(), "error.type").string_value() == "404");
  KJ_EXPECT(fetch.status().code() == otlp::trace::v1::Status::STATUS_CODE_ERROR);
  KJ_EXPECT(fetch.events_size() == 0);
  KJ_EXPECT(fetch.links_size() == 0);
  KJ_EXPECT(user.name() == "renamed-work");
  KJ_EXPECT(user.status().message() == "explicit");
  KJ_ASSERT(user.events_size() == 1);
  KJ_EXPECT(user.events(0).name() == "exception");
  KJ_EXPECT(attribute(user.events(0).attributes(), "exception.type").string_value() == "TypeError");
  KJ_EXPECT(setAlarm.kind() == otlp::trace::v1::Span::SPAN_KIND_PRODUCER);
  KJ_EXPECT(setAlarm.parent_span_id() == user.span_id());
  KJ_EXPECT(
      attribute(setAlarm.attributes(), "cloudflare.invocation.sequence.number").int_value() == 4);
}

// Exports a span tree whose root has a `fetch` child to `url`.
kj::Array<const kj::byte> exportFetch(kj::StringPtr url, RedactQueryString redactQueryString) {
  setPredictableModeForTest();
  NoEntropySource entropy;
  auto exporter = kj::rc<RecordingExporter>();
  {
    auto root =
        startExportedUserSpan(entropy, exporter.addRef(), tracing::SpanContext(TRACE_ID, kj::none),
            {.serviceName = "url-test", .redactQueryString = redactQueryString}, "root"_kjc);
    auto fetch = root.newChild("fetch"_kjc);
    fetch.setTag("url.full"_kjc, url);
  }
  KJ_ASSERT(exporter->spanRequests.size() == 1);
  return kj::mv(exporter->spanRequests[0]);
}

constexpr kj::StringPtr URL_UNREDACTED =
    "0aa9030a1c0a1a0a0c736572766963652e6e616d65120a0a0875726c2d746573741288030a110a0f776f726b"
    "6572732d72756e74696d65129f020a1001020304050607081112131415161718120800000000000000022208"
    "00000000000000012a05666574636830034a3c0a0875726c2e66756c6c12300a2e68747470733a2f2f757365"
    "723a70617373406578616d706c652e636f6d2f612f623f713d3126723d3223667261674a150a0a75726c2e73"
    "6368656d6512070a0568747470734a1f0a0e7365727665722e61646472657373120d0a0b6578616d706c652e"
    "636f6d4a120a0875726c2e7061746812060a042f612f624a1f0a156e6574776f726b2e70726f746f636f6c2e"
    "6e616d6512060a04687474704a160a0975726c2e717565727912090a07713d3126723d324a2b0a25636c6f75"
    "64666c6172652e696e766f636174696f6e2e73657175656e63652e6e756d6265721202180212510a10010203"
    "04050607081112131415161718120800000000000000012a04726f6f7430024a2b0a25636c6f7564666c6172"
    "652e696e766f636174696f6e2e73657175656e63652e6e756d62657212021801"_kj;
constexpr kj::StringPtr URL_REDACTED =
    "0afa020a1c0a1a0a0c736572766963652e6e616d65120a0a0875726c2d7465737412d9020a110a0f776f726b"
    "6572732d72756e74696d6512f0010a1001020304050607081112131415161718120800000000000000022208"
    "00000000000000012a05666574636830034a250a0875726c2e66756c6c12190a1768747470733a2f2f657861"
    "6d706c652e636f6d2f612f624a150a0a75726c2e736368656d6512070a0568747470734a1f0a0e7365727665"
    "722e61646472657373120d0a0b6578616d706c652e636f6d4a120a0875726c2e7061746812060a042f612f62"
    "4a1f0a156e6574776f726b2e70726f746f636f6c2e6e616d6512060a04687474704a2b0a25636c6f7564666c"
    "6172652e696e766f636174696f6e2e73657175656e63652e6e756d6265721202180212510a10010203040506"
    "07081112131415161718120800000000000000012a04726f6f7430024a2b0a25636c6f7564666c6172652e69"
    "6e766f636174696f6e2e73657175656e63652e6e756d62657212021801"_kj;

KJ_TEST("url.full keeps its query unless the query string is redacted") {
  constexpr auto URL = "https://user:pass@example.com/a/b?q=1&r=2#frag"_kj;
  {
    auto request = expectTraceGolden(
        "URL_UNREDACTED", exportFetch(URL, RedactQueryString::NO), URL_UNREDACTED);
    auto& fetch = spans(request)[0];
    KJ_EXPECT(attribute(fetch.attributes(), "url.full").string_value() == URL.cStr());
    KJ_EXPECT(attribute(fetch.attributes(), "url.query").string_value() == "q=1&r=2");
    KJ_EXPECT(attribute(fetch.attributes(), "url.path").string_value() == "/a/b");
    KJ_EXPECT(attribute(fetch.attributes(), "server.address").string_value() == "example.com");
    KJ_EXPECT(!hasAttribute(fetch, "server.port"));
  }
  {
    auto request =
        expectTraceGolden("URL_REDACTED", exportFetch(URL, RedactQueryString::YES), URL_REDACTED);
    auto& fetch = spans(request)[0];
    // The credentials go with the query and the fragment.
    KJ_EXPECT(
        attribute(fetch.attributes(), "url.full").string_value() == "https://example.com/a/b");
    KJ_EXPECT(!hasAttribute(fetch, "url.query"));
  }
}

constexpr kj::StringPtr REFUSED_SPANS =
    "0ace020a200a1e0a0c736572766963652e6e616d65120e0a0c7265667573616c2d7465737412a9020a110a0f"
    "776f726b6572732d72756e74696d65125d0a1001020304050607081112131415161718120800000000000000"
    "03220800000000000000022a066f727068616e30034a2b0a25636c6f7564666c6172652e696e766f63617469"
    "6f6e2e73657175656e63652e6e756d62657212021802125f0a10010203040506070811121314151617181208"
    "0000000000000005220800000000000000012a08616363657074656430034a2b0a25636c6f7564666c617265"
    "2e696e766f636174696f6e2e73657175656e63652e6e756d6265721202180312540a10010203040506070811"
    "12131415161718120800000000000000012a077265667573656430024a2b0a25636c6f7564666c6172652e69"
    "6e766f636174696f6e2e73657175656e63652e6e756d62657212021801"_kj;

KJ_TEST("spans the exporter refuses are left out, but never the root") {
  setPredictableModeForTest();
  NoEntropySource entropy;
  auto exporter = kj::rc<RecordingExporter>();
  exporter->refusedRuntimeSpan = "refused";
  exporter->refusedUserSpan = "refused";
  {
    // Without trace flags, and without a caller's span.
    auto root = startExportedUserSpan(entropy, exporter.addRef(),
        tracing::SpanContext(TRACE_ID, kj::none), {.serviceName = "refusal-test"}, "refused"_kjc);
    {
      auto runtime = root.newChild("refused"_kjc);
      // The child of a refused span is exported, under the refused span's id.
      auto orphan = runtime.newChild("orphan"_kjc);
    }
    {
      auto refused =
          SpanBuilder(KJ_ASSERT_NONNULL(root.getObserver()).newChildFromUserCode(), "refused"_kjc);
      auto accepted =
          SpanBuilder(KJ_ASSERT_NONNULL(root.getObserver()).newChildFromUserCode(), "accepted"_kjc);
    }
  }
  // Refusals are not drops.
  KJ_EXPECT(exporter->dropped == 0);
  KJ_ASSERT(exporter->spanRequests.size() == 1);
  KJ_EXPECT(exporter->spanCounts[0] == 3);
  auto request = expectTraceGolden("REFUSED_SPANS", exporter->spanRequests[0], REFUSED_SPANS);
  auto& out = spans(request);
  KJ_ASSERT(out.size() == 3);
  KJ_EXPECT(out[0].name() == "orphan");
  // Span ids count from 1 in predictable mode, so the refused span was 2.
  KJ_EXPECT(out[0].parent_span_id() == spanId(2));
  KJ_EXPECT(out[1].name() == "accepted");
  KJ_EXPECT(out[2].name() == "refused");
  KJ_EXPECT(out[2].span_id() == spanId(1));
  KJ_EXPECT(out[2].parent_span_id().empty());
  KJ_EXPECT(out[2].flags() == 0);
  // The accepted spans number on from the root, skipping the refused ones.
  KJ_EXPECT(
      attribute(out[0].attributes(), "cloudflare.invocation.sequence.number").int_value() == 2);
  KJ_EXPECT(
      attribute(out[1].attributes(), "cloudflare.invocation.sequence.number").int_value() == 3);
}

tracing::SpanContext testContext() {
  return tracing::SpanContext(TRACE_ID, kj::none, tracing::TraceFlags(1));
}

constexpr kj::StringPtr DUPLICATE_OPEN =
    "0ad0020a170a150a0c736572766963652e6e616d6512050a0364757012b4020a110a0f776f726b6572732d72"
    "756e74696d6512620a1001020304050607081112131415161718120800000000000000052208000000000000"
    "00012a05666972737430034a2b0a25636c6f7564666c6172652e696e766f636174696f6e2e73657175656e63"
    "652e6e756d6265721202180285010100000012620a1001020304050607081112131415161718120800000000"
    "00000006220800000000000000012a05746869726430034a2b0a25636c6f7564666c6172652e696e766f6361"
    "74696f6e2e73657175656e63652e6e756d6265721202180385010100000012570a1001020304050607081112"
    "131415161718120800000000000000012a04726f6f7430024a2b0a25636c6f7564666c6172652e696e766f63"
    "6174696f6e2e73657175656e63652e6e756d62657212021801850101000000"_kj;

KJ_TEST("opening a span id that is already open drops the span") {
  setPredictableModeForTest();
  NoEntropySource entropy;
  auto exporter = kj::rc<RecordingExporter>();
  {
    auto submitter = kj::refcounted<OtlpSpanSubmitter>(entropy, exporter.addRef(), testContext(),
        OtlpSpanSubmitter::Options{.serviceName = "dup"});
    auto root = submitter->getRootSpanId();
    KJ_EXPECT(root == tracing::SpanId(1));
    KJ_EXPECT(submitter->submitSpanOpen(root, tracing::SpanId::nullId, "root"_kjc, kj::UNIX_EPOCH));
    KJ_EXPECT(submitter->submitSpanOpen(tracing::SpanId(5), root, "first"_kjc, kj::UNIX_EPOCH));
    KJ_EXPECT(!submitter->submitSpanOpen(tracing::SpanId(5), root, "second"_kjc, kj::UNIX_EPOCH));
    KJ_EXPECT(exporter->dropped == 1);
    KJ_EXPECT(submitter->submitSpanOpen(tracing::SpanId(6), root, "third"_kjc, kj::UNIX_EPOCH));
    submitter->submitSpanClose(tracing::SpanId(5), kj::UNIX_EPOCH, kj::UNIX_EPOCH, {});
    // A second close, and updates of a span that is not open, are ignored.
    submitter->submitSpanClose(tracing::SpanId(5), kj::UNIX_EPOCH, kj::UNIX_EPOCH, {});
    submitter->submitSpanUpdate(tracing::SpanId(5), tracing::SpanUpdate("late"_kjc));
    submitter->submitSpanClose(tracing::SpanId(6), kj::UNIX_EPOCH, kj::UNIX_EPOCH, {});
    submitter->submitSpanClose(root, kj::UNIX_EPOCH, kj::UNIX_EPOCH, {});
  }
  KJ_ASSERT(exporter->spanRequests.size() == 1);
  KJ_EXPECT(exporter->spanCounts[0] == 3);
  auto request = expectTraceGolden("DUPLICATE_OPEN", exporter->spanRequests[0], DUPLICATE_OPEN);
  auto& out = spans(request);
  KJ_ASSERT(out.size() == 3);
  KJ_EXPECT(out[0].name() == "first");
  KJ_EXPECT(out[1].name() == "third");
  // The refused open takes no sequence number.
  KJ_EXPECT(
      attribute(out[1].attributes(), "cloudflare.invocation.sequence.number").int_value() == 3);
}

KJ_TEST("the 1,025th open span is dropped, and batches hold at most 512 spans") {
  setPredictableModeForTest();
  NoEntropySource entropy;
  auto exporter = kj::rc<RecordingExporter>();
  {
    auto submitter = kj::refcounted<OtlpSpanSubmitter>(entropy, exporter.addRef(), testContext(),
        OtlpSpanSubmitter::Options{.serviceName = "limits"});
    auto root = submitter->getRootSpanId();
    KJ_EXPECT(submitter->submitSpanOpen(root, tracing::SpanId::nullId, "root"_kjc, kj::UNIX_EPOCH));
    for (uint64_t id = 2; id <= 1024; ++id) {
      KJ_ASSERT(submitter->submitSpanOpen(tracing::SpanId(id), root, "child"_kjc, kj::UNIX_EPOCH));
    }
    KJ_EXPECT(exporter->dropped == 0);
    KJ_EXPECT(!submitter->submitSpanOpen(tracing::SpanId(1025), root, "child"_kjc, kj::UNIX_EPOCH));
    KJ_EXPECT(exporter->dropped == 1);
    for (uint64_t id = 2; id <= 1024; ++id) {
      submitter->submitSpanClose(tracing::SpanId(id), kj::UNIX_EPOCH, kj::UNIX_EPOCH, {});
      // A full batch goes out without waiting for the root.
      KJ_EXPECT(exporter->spanRequests.size() == (id < 513 ? 0 : 1));
    }
    // Once spans have closed there is room for another, whose close fills the second batch.
    KJ_EXPECT(submitter->submitSpanOpen(tracing::SpanId(1025), root, "child"_kjc, kj::UNIX_EPOCH));
    submitter->submitSpanClose(tracing::SpanId(1025), kj::UNIX_EPOCH, kj::UNIX_EPOCH, {});
    KJ_EXPECT(exporter->spanRequests.size() == 2);
    submitter->submitSpanClose(root, kj::UNIX_EPOCH, kj::UNIX_EPOCH, {});
  }
  KJ_ASSERT(exporter->spanCounts.size() == 3);
  KJ_EXPECT(exporter->spanCounts[0] == 512);
  KJ_EXPECT(exporter->spanCounts[1] == 512);
  KJ_EXPECT(exporter->spanCounts[2] == 1);
  for (auto i: kj::indices(exporter->spanRequests)) {
    TraceRequest request;
    KJ_ASSERT(request.ParseFromArray(
        exporter->spanRequests[i].begin(), exporter->spanRequests[i].size()));
    KJ_EXPECT(spans(request).size() == exporter->spanCounts[i]);
  }
}

constexpr kj::StringPtr AFTER_ROOT_ROOT =
    "0a8e010a1e0a1c0a0c736572766963652e6e616d65120c0a0a61667465722d726f6f74126c0a110a0f776f72"
    "6b6572732d72756e74696d6512570a1001020304050607081112131415161718120800000000000000012a04"
    "726f6f7430024a2b0a25636c6f7564666c6172652e696e766f636174696f6e2e73657175656e63652e6e756d"
    "62657212021801850101000000"_kj;
constexpr kj::StringPtr AFTER_ROOT_LATE =
    "0abc010a1e0a1c0a0c736572766963652e6e616d65120c0a0a61667465722d726f6f741299010a110a0f776f"
    "726b6572732d72756e74696d651283010a100102030405060708111213141516171812080000000000000002"
    "220800000000000000012a0a776169745f756e74696c30034a1a0a14636c6f7564666c6172652e746573742e"
    "6c617465120210014a2b0a25636c6f7564666c6172652e696e766f636174696f6e2e73657175656e63652e6e"
    "756d62657212021802850101000000"_kj;

KJ_TEST("spans that close after the root are exported one at a time") {
  setPredictableModeForTest();
  NoEntropySource entropy;
  auto exporter = kj::rc<RecordingExporter>();
  {
    auto root = startExportedUserSpan(entropy, exporter.addRef(), testContext(),
        OtlpSpanSubmitter::Options{.serviceName = "after-root"}, "root"_kjc);
    auto waitUntil = root.newChild("wait_until"_kjc);
    auto handoff = root.newChild("handoff"_kjc);
    root.end();
    KJ_ASSERT(exporter->spanRequests.size() == 1);
    waitUntil.setTag("cloudflare.test.late"_kjc, true);
    waitUntil.end();
    KJ_ASSERT(exporter->spanRequests.size() == 2);
    handoff.end();
    KJ_ASSERT(exporter->spanRequests.size() == 3);
  }
  KJ_EXPECT(exporter->spanCounts.size() == 3);
  for (auto count: exporter->spanCounts) KJ_EXPECT(count == 1);
  auto root = expectTraceGolden("AFTER_ROOT_ROOT", exporter->spanRequests[0], AFTER_ROOT_ROOT);
  KJ_EXPECT(spans(root)[0].name() == "root");
  auto late = expectTraceGolden("AFTER_ROOT_LATE", exporter->spanRequests[1], AFTER_ROOT_LATE);
  KJ_EXPECT(spans(late)[0].name() == "wait_until");
  // Each request carries the resource again.
  KJ_EXPECT(late.resource_spans(0).resource().attributes(0).value().string_value() == "after-root");
}

constexpr kj::StringPtr NOT_ENDED =
    "0ac0060a1d0a1b0a0c736572766963652e6e616d65120b0a096e6f742d656e646564129e060a110a0f776f72"
    "6b6572732d72756e74696d6512620a1001020304050607081112131415161718120800000000000000022208"
    "00000000000000012a05656e64656430034a2b0a25636c6f7564666c6172652e696e766f636174696f6e2e73"
    "657175656e63652e6e756d6265721202180285010100000012d8010a10010203040506070811121314151617"
    "18120800000000000000012a04726f6f7430024a2b0a17636c6f7564666c6172652e7761726e696e672e7479"
    "706512100a0e7370616e5f6e6f745f656e6465644a520a1a636c6f7564666c6172652e7761726e696e672e6d"
    "65737361676512340a32546865207370616e20776173207374696c6c206f70656e207768656e207468652069"
    "6e766f636174696f6e20656e6465642e4a2b0a25636c6f7564666c6172652e696e766f636174696f6e2e7365"
    "7175656e63652e6e756d6265721202180185010100000012e2010a1001020304050607081112131415161718"
    "12080000000000000003220800000000000000012a046f70656e30034a2b0a17636c6f7564666c6172652e77"
    "61726e696e672e7479706512100a0e7370616e5f6e6f745f656e6465644a520a1a636c6f7564666c6172652e"
    "7761726e696e672e6d65737361676512340a32546865207370616e20776173207374696c6c206f70656e2077"
    "68656e2074686520696e766f636174696f6e20656e6465642e4a2b0a25636c6f7564666c6172652e696e766f"
    "636174696f6e2e73657175656e63652e6e756d6265721202180385010100000012e4010a1001020304050607"
    "08111213141516171812080000000000000004220800000000000000032a066e657374656430034a2b0a1763"
    "6c6f7564666c6172652e7761726e696e672e7479706512100a0e7370616e5f6e6f745f656e6465644a520a1a"
    "636c6f7564666c6172652e7761726e696e672e6d65737361676512340a32546865207370616e207761732073"
    "74696c6c206f70656e207768656e2074686520696e766f636174696f6e20656e6465642e4a2b0a25636c6f75"
    "64666c6172652e696e766f636174696f6e2e73657175656e63652e6e756d62657212021804850101000000"_kj;

KJ_TEST("destroying the submitter ends and exports the spans still open") {
  setPredictableModeForTest();
  NoEntropySource entropy;
  auto exporter = kj::rc<RecordingExporter>();
  {
    auto submitter = kj::refcounted<OtlpSpanSubmitter>(entropy, exporter.addRef(), testContext(),
        OtlpSpanSubmitter::Options{.serviceName = "not-ended"});
    auto root = submitter->getRootSpanId();
    KJ_EXPECT(submitter->submitSpanOpen(root, tracing::SpanId::nullId, "root"_kjc, kj::UNIX_EPOCH));
    auto ended = submitter->makeSpanId();
    auto open = submitter->makeSpanId();
    auto nested = submitter->makeSpanId();
    KJ_EXPECT(submitter->submitSpanOpen(ended, root, "ended"_kjc, kj::UNIX_EPOCH));
    KJ_EXPECT(submitter->submitSpanOpen(open, root, "open"_kjc, kj::UNIX_EPOCH));
    KJ_EXPECT(submitter->submitUserSpanOpen(nested, open, "nested"_kjc, kj::UNIX_EPOCH));
    submitter->submitSpanClose(ended, kj::UNIX_EPOCH, kj::UNIX_EPOCH, {});
    KJ_EXPECT(exporter->spanRequests.size() == 0);
  }
  KJ_ASSERT(exporter->spanRequests.size() == 1);
  KJ_EXPECT(exporter->spanCounts[0] == 4);
  auto request = expectTraceGolden("NOT_ENDED", exporter->spanRequests[0], NOT_ENDED);
  auto& out = spans(request);
  KJ_ASSERT(out.size() == 4);
  KJ_EXPECT(out[0].name() == "ended");
  KJ_EXPECT(!hasAttribute(out[0], "cloudflare.warning.type"));
  // The rest in the order they opened, the root first.
  KJ_EXPECT(out[1].name() == "root");
  KJ_EXPECT(out[2].name() == "open");
  KJ_EXPECT(out[3].name() == "nested");
  for (auto i: {1, 2, 3}) {
    KJ_EXPECT(attribute(out[i].attributes(), "cloudflare.warning.type").string_value() ==
        "span_not_ended");
  }
}

KJ_TEST("an invocation whose spans are not exported exports nothing") {
  setPredictableModeForTest();
  NoEntropySource entropy;
  auto exporter = kj::rc<RecordingExporter>();
  {
    auto started = startExportedInvocationSpan(entropy, exporter.addRef(), testContext(),
        {.serviceName = "no-spans", .exportSpans = ExportSpans::NO},
        tracing::SpanId(INVOCATION_SPAN_ID));
    KJ_EXPECT(started.exportSpans == ExportSpans::NO);
    describeInvocationSpan(started.span, tracing::CustomEventInfo());
    auto child = started.span.newChild("child"_kjc);
    auto user = SpanBuilder(
        KJ_ASSERT_NONNULL(started.span.getObserver()).newChildFromUserCode(), "user"_kjc);
  }
  KJ_EXPECT(exporter->spanRequests.size() == 0);
  // The other spans are refused without counting as drops, after the exporter was asked.
  KJ_EXPECT(exporter->dropped == 0);
  KJ_EXPECT(exporter->runtimeSpansAsked.size() == 1);
  KJ_EXPECT(exporter->userSpansAsked.size() == 1);
}

constexpr kj::StringPtr SPAN_SPACE =
    "0afa050ad3010a1c0a0c736572766963652e6e616d65120c0a0a7370616e2d73706163650a270a1274656c65"
    "6d657472792e73646b2e6e616d6512110a0f776f726b6572732d72756e74696d650a250a16636c6f7564666c"
    "6172652e7363726970745f6e616d65120b0a096d792d776f726b65720a220a13636c6f7564666c6172652e74"
    "6573742e696e74120b18f9ffffffffffffffff010a230a16636c6f7564666c6172652e746573742e646f7562"
    "6c65120921000000000000f83f0a1a0a14636c6f7564666c6172652e746573742e626f6f6c1202100112a104"
    "0a110a0f776f726b6572732d72756e74696d6512b7010a100102030405060708111213141516171812080100"
    "000000000002220801000000000000012a05666574636830034a270a15636c6f7564666c6172652e656e7472"
    "79706f696e74120e0a0c4d79456e747279706f696e744a2a0a1c636c6f7564666c6172652e64757261626c65"
    "5f6f626a6563742e6964120a0a0830313233616263644a2b0a25636c6f7564666c6172652e696e766f636174"
    "696f6e2e73657175656e63652e6e756d6265721202180285010100000012d1020a1001020304050607081112"
    "1314151617181208010000000000000122080000000000ca11e52a05636163686530024a280a0875726c2e66"
    "756c6c121c0a1a68747470733a2f2f6578616d706c652e636f6d2f6361636865644a150a0a75726c2e736368"
    "656d6512070a0568747470734a1f0a0e7365727665722e61646472657373120d0a0b6578616d706c652e636f"
    "6d4a150a0875726c2e7061746812090a072f6361636865644a1f0a156e6574776f726b2e70726f746f636f6c"
    "2e6e616d6512060a04687474704a270a15636c6f7564666c6172652e656e747279706f696e74120e0a0c4d79"
    "456e747279706f696e744a2a0a1c636c6f7564666c6172652e64757261626c655f6f626a6563742e6964120a"
    "0a0830313233616263644a2b0a25636c6f7564666c6172652e696e766f636174696f6e2e73657175656e6365"
    "2e6e756d62657212021801850101000000"_kj;

KJ_TEST("predictableSpanSpace sets the top byte of predictable span ids") {
  setPredictableModeForTest();
  NoEntropySource entropy;
  auto exporter = kj::rc<RecordingExporter>();
  auto resource = testResource();
  auto defaultTags = testDefaultTags();
  {
    // A user span that continues a caller's sampled trace, as embedders open spans outside an
    // invocation.
    auto root = startExportedUserSpan(entropy, exporter.addRef(),
        tracing::SpanContext(TRACE_ID, tracing::SpanId(CALLER_SPAN_ID), tracing::TraceFlags(1)),
        {
          .serviceName = "span-space",
          .resource = resource,
          .defaultTags = defaultTags,
          .redactQueryString = RedactQueryString::YES,
          .predictableSpanSpace = 1,
        },
        "cache"_kjc);
    root.setTag("url.full"_kjc, "https://example.com/cached?key=1"_kj);
    auto child = root.newChild("fetch"_kjc);
  }
  KJ_ASSERT(exporter->spanRequests.size() == 1);
  auto request = expectTraceGolden("SPAN_SPACE", exporter->spanRequests[0], SPAN_SPACE);
  auto& out = spans(request);
  KJ_ASSERT(out.size() == 2);
  KJ_EXPECT(out[0].span_id() == spanId(0x0100000000000002));
  KJ_EXPECT(out[0].parent_span_id() == spanId(0x0100000000000001));
  KJ_EXPECT(out[1].span_id() == spanId(0x0100000000000001));
  KJ_EXPECT(out[1].parent_span_id() == spanId(CALLER_SPAN_ID));
  KJ_EXPECT(
      attribute(out[1].attributes(), "url.full").string_value() == "https://example.com/cached");
}

// Errors in two of a console call's three arguments, one with a stack and one without.
tracing::LogErrorInfo mixedErrorInfo() {
  auto slots = kj::heapArrayBuilder<kj::Maybe<tracing::ErrorInfo>>(3);
  slots.add(kj::none);
  slots.add(
      tracing::ErrorInfo(kj::str("TypeError"), kj::str("bad"), kj::str("    at f (a.js:1:1)")));
  slots.add(tracing::ErrorInfo(kj::str("RangeError"), kj::str("far"), kj::none));
  return slots.finish();
}

constexpr kj::StringPtr INVOCATION_LOGS =
    "0a98110ad2010a1b0a0c736572766963652e6e616d65120b0a096d792d776f726b65720a270a1274656c656d"
    "657472792e73646b2e6e616d6512110a0f776f726b6572732d72756e74696d650a250a16636c6f7564666c61"
    "72652e7363726970745f6e616d65120b0a096d792d776f726b65720a220a13636c6f7564666c6172652e7465"
    "73742e696e74120b18f9ffffffffffffffff010a230a16636c6f7564666c6172652e746573742e646f75626c"
    "65120921000000000000f83f0a1a0a14636c6f7564666c6172652e746573742e626f6f6c1202100112c00f0a"
    "110a0f776f726b6572732d72756e74696d6512c50210091a036c6f672a2b2a290a070a0568656c6c6f0a1a32"
    "180a070a0162120218010a0d0a016112082a060a0210010a000a0232003298010a19636c6f7564666c617265"
    "2e6c6f672e6572726f725f696e666f127b2a790a000a4932470a130a046e616d65120b0a0954797065457272"
    "6f720a100a076d65737361676512050a036261640a1e0a05737461636b12150a132020202061742066202861"
    "2e6a733a313a31290a2a32280a140a046e616d65120c0a0a52616e67654572726f720a100a076d6573736167"
    "6512050a0366617232270a15636c6f7564666c6172652e656e747279706f696e74120e0a0c4d79456e747279"
    "706f696e74322a0a1c636c6f7564666c6172652e64757261626c655f6f626a6563742e6964120a0a08303132"
    "336162636445010000004a10010203040506070811121314151617185208000000000000001d12ab01100d1a"
    "047761726e2a0b0a095b22637574206f6666321e0a18636c6f7564666c6172652e6c6f672e7472756e636174"
    "65641202100132270a15636c6f7564666c6172652e656e747279706f696e74120e0a0c4d79456e747279706f"
    "696e74322a0a1c636c6f7564666c6172652e64757261626c655f6f626a6563742e6964120a0a083031323361"
    "62636445010000004a10010203040506070811121314151617185208000000000000001d128e0110051a0564"
    "656275672a0d2a0b0a0921000000000000f83f32270a15636c6f7564666c6172652e656e747279706f696e74"
    "120e0a0c4d79456e747279706f696e74322a0a1c636c6f7564666c6172652e64757261626c655f6f626a6563"
    "742e6964120a0a08303132336162636445010000004a10010203040506070811121314151617185208000000"
    "000000000112830110091a04696e666f2a0d2a0b0a090a076e6f207370616e32270a15636c6f7564666c6172"
    "652e656e747279706f696e74120e0a0c4d79456e747279706f696e74322a0a1c636c6f7564666c6172652e64"
    "757261626c655f6f626a6563742e6964120a0a08303132336162636445010000004a10010203040506070811"
    "1213141516171812fa0110111a056572726f722a060a04626f6f6d32190a0e657863657074696f6e2e747970"
    "6512070a054572726f72321b0a11657863657074696f6e2e6d65737361676512060a04626f6f6d322e0a1465"
    "7863657074696f6e2e737461636b747261636512160a144572726f723a20626f6f6d0a202020206174206732"
    "270a15636c6f7564666c6172652e656e747279706f696e74120e0a0c4d79456e747279706f696e74322a0a1c"
    "636c6f7564666c6172652e64757261626c655f6f626a6563742e6964120a0a08303132336162636445010000"
    "004a10010203040506070811121314151617185208000000000000001d6209657863657074696f6e12d60110"
    "111a056572726f722a0a0a086e6f20737461636b321d0a0e657863657074696f6e2e74797065120b0a095479"
    "70654572726f72321f0a11657863657074696f6e2e6d657373616765120a0a086e6f20737461636b32270a15"
    "636c6f7564666c6172652e656e747279706f696e74120e0a0c4d79456e747279706f696e74322a0a1c636c6f"
    "7564666c6172652e64757261626c655f6f626a6563742e6964120a0a08303132336162636445010000004a10"
    "010203040506070811121314151617185208000000000000001d6209657863657074696f6e12c7052a050a03"
    "47455432160a0c666161732e7472696767657212060a046874747032220a17636c6f7564666c6172652e6861"
    "6e646c65725f7479706512070a056665746368321c0a13687474702e726571756573742e6d6574686f641205"
    "0a0347455432260a0875726c2e66756c6c121a0a1868747470733a2f2f6578616d706c652e636f6d2f706174"
    "6832210a13757365725f6167656e742e6f726967696e616c120a0a08746573742f312e3032150a0e636c6f75"
    "64666c6172652e61736e120318976832180a1067656f2e636f756e7472792e636f646512040a025054321d0a"
    "1167656f2e6c6f63616c6974792e6e616d6512080a064c6973626f6e321a0a1267656f2e636f6e74696e656e"
    "742e636f646512040a02455532200a19687474702e726573706f6e73652e7374617475735f636f6465120318"
    "f40332210a12636c6f7564666c6172652e6f7574636f6d65120b0a09657863657074696f6e32110a0b637075"
    "5f74696d655f6d731202180032120a0c77616c6c5f74696d655f6d731202180032150a0a75726c2e73636865"
    "6d6512070a056874747073321f0a0e7365727665722e61646472657373120d0a0b6578616d706c652e636f6d"
    "32130a0875726c2e7061746812070a052f70617468321f0a156e6574776f726b2e70726f746f636f6c2e6e61"
    "6d6512060a046874747032270a15636c6f7564666c6172652e656e747279706f696e74120e0a0c4d79456e74"
    "7279706f696e74322a0a1c636c6f7564666c6172652e64757261626c655f6f626a6563742e6964120a0a0830"
    "31323361626364322b0a25636c6f7564666c6172652e696e766f636174696f6e2e73657175656e63652e6e75"
    "6d6265721202180132190a0a6572726f722e74797065120b0a09657863657074696f6e45010000004a100102"
    "03040506070811121314151617185208000000000000001d6215636c6f7564666c6172652e696e766f636174"
    "696f6e"_kj;
constexpr kj::StringPtr LATE_LOG =
    "0af9020ad2010a1b0a0c736572766963652e6e616d65120b0a096d792d776f726b65720a270a1274656c656d"
    "657472792e73646b2e6e616d6512110a0f776f726b6572732d72756e74696d650a250a16636c6f7564666c61"
    "72652e7363726970745f6e616d65120b0a096d792d776f726b65720a220a13636c6f7564666c6172652e7465"
    "73742e696e74120b18f9ffffffffffffffff010a230a16636c6f7564666c6172652e746573742e646f75626c"
    "65120921000000000000f83f0a1a0a14636c6f7564666c6172652e746573742e626f6f6c1202100112a1010a"
    "110a0f776f726b6572732d72756e74696d65128b0110111a056572726f722a0a2a080a060a046c6174653227"
    "0a15636c6f7564666c6172652e656e747279706f696e74120e0a0c4d79456e747279706f696e74322a0a1c63"
    "6c6f7564666c6172652e64757261626c655f6f626a6563742e6964120a0a0830313233616263644501000000"
    "4a10010203040506070811121314151617185208000000000000001d"_kj;

KJ_TEST("an invocation's logs go out with its root span, with a record of the invocation") {
  setPredictableModeForTest();
  NoEntropySource entropy;
  auto exporter = kj::rc<RecordingExporter>();
  auto resource = testResource();
  auto defaultTags = testDefaultTags();
  kj::Maybe<kj::Own<InvocationLogSubmitter>> logs;
  {
    auto started = startExportedInvocationSpan(entropy, exporter.addRef(),
        tracing::SpanContext(TRACE_ID, tracing::SpanId(CALLER_SPAN_ID), tracing::TraceFlags(1)),
        {
          .serviceName = "my-worker",
          .resource = resource,
          .defaultTags = defaultTags,
          .redactQueryString = RedactQueryString::YES,
          .exportLogs = ExportLogs::YES,
          .exportSpans = ExportSpans::YES,
          .exportInvocationLog = ExportInvocationLog::YES,
        },
        tracing::SpanId(INVOCATION_SPAN_ID));
    auto& submitter = *KJ_ASSERT_NONNULL(started.logs);
    auto invocation = tracing::SpanId(INVOCATION_SPAN_ID);
    auto time = kj::UNIX_EPOCH + 5 * kj::SECONDS;
    describeInvocationSpan(started.span, testFetchEvent());
    submitter.submitLog(invocation, time, LogLevel::LOG,
        "[\"hello\",{\"b\":1,\"a\":[true,null]},{}]"_kj, tracing::LogTruncated::NO,
        mixedErrorInfo());
    // A truncated message is not JSON, so it becomes a string body.
    submitter.submitLog(
        invocation, time, LogLevel::WARN, "[\"cut off"_kj, tracing::LogTruncated::YES, kj::none);
    {
      auto child = started.span.newChild("fetch"_kjc);
      auto childId = KJ_ASSERT_NONNULL(child.getObserver()).getSpanId();
      submitter.submitLog(
          childId, time, LogLevel::DEBUG_, "[1.5]"_kj, tracing::LogTruncated::NO, kj::none);
    }
    // A log without a span.
    submitter.submitLog(tracing::SpanId::nullId, time, LogLevel::INFO, "[\"no span\"]"_kj,
        tracing::LogTruncated::NO, kj::none);
    submitter.submitException(invocation, time, "Error"_kj, "boom"_kj, "Error: boom\n    at g"_kj);
    submitter.submitException(invocation, time, "TypeError"_kj, "no stack"_kj, kj::none);
    setInvocationResponseStatus(started.span, 500);
    setInvocationOutcome(
        started.span, EventOutcome::EXCEPTION, 1 * kj::MILLISECONDS, 2 * kj::MILLISECONDS);
    // Logs wait for the root span.
    KJ_EXPECT(exporter->logRequests.size() == 0);
    logs = kj::mv(started.logs);
  }
  KJ_ASSERT(exporter->spanRequests.size() == 1);
  KJ_ASSERT(exporter->logRequests.size() == 1);
  KJ_EXPECT(exporter->logCounts[0] == 7);
  // After the root closed, each log goes out on its own.
  KJ_ASSERT_NONNULL(logs)->submitLog(tracing::SpanId(INVOCATION_SPAN_ID), kj::UNIX_EPOCH,
      LogLevel::ERROR, "[\"late\"]"_kj, tracing::LogTruncated::NO, kj::none);
  KJ_ASSERT(exporter->logRequests.size() == 2);
  KJ_EXPECT(exporter->logCounts[1] == 1);
  logs = kj::none;
  KJ_EXPECT(exporter->logRequests.size() == 2);

  auto request = expectLogsGolden("INVOCATION_LOGS", exporter->logRequests[0], INVOCATION_LOGS);
  auto& resourceAttributes = request.resource_logs(0).resource().attributes();
  KJ_EXPECT(resourceAttributes.size() == 6);
  KJ_EXPECT(resourceAttributes[0].value().string_value() == "my-worker");
  auto& out = records(request);
  KJ_ASSERT(out.size() == 7);
  auto& log = out[0];
  KJ_EXPECT(log.severity_text() == "log");
  KJ_EXPECT(log.time_unix_nano() == 0);
  KJ_EXPECT(log.span_id() == spanId(INVOCATION_SPAN_ID));
  auto traceId = TRACE_ID.toProtobuf();
  KJ_EXPECT(log.trace_id() == std::string(traceId.asChars().begin(), traceId.size()));
  KJ_EXPECT(log.flags() == 1);
  // Object keys keep their order.
  KJ_EXPECT(log.body().array_value().values(1).kvlist_value().values(0).key() == "b");
  auto& errorInfo = attribute(log.attributes(), "cloudflare.log.error_info").array_value();
  KJ_ASSERT(errorInfo.values_size() == 3);
  KJ_EXPECT(errorInfo.values(0).value_case() == otlp::common::v1::AnyValue::VALUE_NOT_SET);
  KJ_EXPECT(errorInfo.values(1).kvlist_value().values_size() == 3);
  KJ_EXPECT(errorInfo.values(2).kvlist_value().values_size() == 2);
  KJ_EXPECT(attribute(log.attributes(), "cloudflare.entrypoint").string_value() == "MyEntrypoint");
  auto& truncated = out[1];
  KJ_EXPECT(truncated.severity_text() == "warn");
  KJ_EXPECT(truncated.body().string_value() == "[\"cut off");
  KJ_EXPECT(attribute(truncated.attributes(), "cloudflare.log.truncated").bool_value());
  KJ_EXPECT(out[2].severity_text() == "debug");
  KJ_EXPECT(out[2].span_id() == spanId(1));
  KJ_EXPECT(out[3].span_id().empty());
  KJ_EXPECT(out[4].event_name() == "exception");
  KJ_EXPECT(attribute(out[4].attributes(), "exception.stacktrace").string_value() ==
      "Error: boom\n    at g");
  KJ_EXPECT(out[5].event_name() == "exception");
  auto& invocationRecord = out[6];
  KJ_EXPECT(invocationRecord.event_name() == "cloudflare.invocation");
  KJ_EXPECT(invocationRecord.body().string_value() == "GET");
  KJ_EXPECT(invocationRecord.span_id() == spanId(INVOCATION_SPAN_ID));
  KJ_EXPECT(
      attribute(invocationRecord.attributes(), "cloudflare.outcome").string_value() == "exception");
  // The root span's attributes as exported, so with the query redacted.
  KJ_EXPECT(attribute(invocationRecord.attributes(), "url.full").string_value() ==
      "https://example.com/path");
  KJ_EXPECT(attribute(invocationRecord.attributes(), "error.type").string_value() == "exception");

  auto late = expectLogsGolden("LATE_LOG", exporter->logRequests[1], LATE_LOG);
  KJ_EXPECT(records(late)[0].severity_text() == "error");
}

constexpr kj::StringPtr UNSAMPLED_INVOCATION_LOGS =
    "0aad030a1d0a1b0a0c736572766963652e6e616d65120b0a09756e73616d706c6564128b030a110a0f776f72"
    "6b6572732d72756e74696d65122f10091a036c6f672a0a2a080a060a047469636b4a10010203040506070811"
    "121314151617185208000000000000001d12c4022a0b0a097363686564756c656432170a0c666161732e7472"
    "696767657212070a0574696d657232260a17636c6f7564666c6172652e68616e646c65725f74797065120b0a"
    "097363686564756c656432180a09666161732e63726f6e120b0a092a202a202a202a202a32370a19636c6f75"
    "64666c6172652e7363686564756c65645f74696d65121a0a18313937302d30312d30315430303a30303a3030"
    "2e3030305a321a0a12636c6f7564666c6172652e6f7574636f6d6512040a026f6b32110a0b6370755f74696d"
    "655f6d731202180032120a0c77616c6c5f74696d655f6d7312021800322b0a25636c6f7564666c6172652e69"
    "6e766f636174696f6e2e73657175656e63652e6e756d626572120218014a1001020304050607081112131415"
    "1617185208000000000000001d6215636c6f7564666c6172652e696e766f636174696f6e"_kj;

KJ_TEST("an invocation whose spans are not exported still exports its logs and its record") {
  setPredictableModeForTest();
  NoEntropySource entropy;
  auto exporter = kj::rc<RecordingExporter>();
  {
    auto started = startExportedInvocationSpan(entropy, exporter.addRef(),
        tracing::SpanContext(TRACE_ID, kj::none, tracing::TraceFlags(0)),
        {
          .serviceName = "unsampled",
          .exportLogs = ExportLogs::YES,
          .exportSpans = ExportSpans::NO,
          .exportInvocationLog = ExportInvocationLog::YES,
        },
        tracing::SpanId(INVOCATION_SPAN_ID));
    describeInvocationSpan(started.span, tracing::ScheduledEventInfo(0, kj::str("* * * * *")));
    KJ_ASSERT_NONNULL(started.logs)
        ->submitLog(tracing::SpanId(INVOCATION_SPAN_ID), kj::UNIX_EPOCH, LogLevel::LOG,
            "[\"tick\"]"_kj, tracing::LogTruncated::NO, kj::none);
    setInvocationOutcome(
        started.span, EventOutcome::OK, 0 * kj::MILLISECONDS, 0 * kj::MILLISECONDS);
  }
  KJ_EXPECT(exporter->spanRequests.size() == 0);
  KJ_ASSERT(exporter->logRequests.size() == 1);
  KJ_EXPECT(exporter->logCounts[0] == 2);
  auto request = expectLogsGolden(
      "UNSAMPLED_INVOCATION_LOGS", exporter->logRequests[0], UNSAMPLED_INVOCATION_LOGS);
  auto& out = records(request);
  KJ_ASSERT(out.size() == 2);
  KJ_EXPECT(out[0].flags() == 0);
  KJ_EXPECT(out[1].event_name() == "cloudflare.invocation");
  KJ_EXPECT(out[1].body().string_value() == "scheduled");
  KJ_EXPECT(attribute(out[1].attributes(), "faas.trigger").string_value() == "timer");
}

}  // namespace
}  // namespace workerd
