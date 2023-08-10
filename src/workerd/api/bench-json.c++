#include <benchmark/benchmark.h>
#include "capnp/compat/json.h"
#include <capnp/compat/json.capnp.h>
#include <workerd/tests/bench-tools.h>
#include <kj/debug.h>
#include <kj/string.h>
#include <kj/test.h>
#include <workerd/api/r2-api.capnp.h>

static void Test_JSON_ENC(benchmark::State& state) {
  // Example test, derived from capnproto's json test.
  capnp::JsonCodec json;
  // Perform setup here

  for (auto _ : state) {
    // This code gets timed
    KJ_EXPECT(json.encode(capnp::VOID) == "null");
    KJ_EXPECT(json.encode(true) == "true");
    KJ_EXPECT(json.encode(false) == "false");
    KJ_EXPECT(json.encode(123) == "123");
    KJ_EXPECT(json.encode(-5.5) == "-5.5");
    KJ_EXPECT(json.encode(capnp::Text::Reader("foo")) == "\"foo\"");
    KJ_EXPECT(json.encode(capnp::Text::Reader("ab\"cd\\ef\x03")) == "\"ab\\\"cd\\\\ef\\u0003\"");
    // TODO
    //KJ_EXPECT(json.encode(capnproto_test::capnp::test::TestEnum::CORGE) == "\"corge\"");

    json.setPrettyPrint(false);
    kj::byte bytes[] = {12, 34, 56};
    KJ_EXPECT(json.encode(capnp::Data::Reader(bytes, 3)) == "[12,34,56]");

    json.setPrettyPrint(true);
    KJ_EXPECT(json.encode(capnp::Data::Reader(bytes, 3)) == "[12, 34, 56]");
  }
}

static void Test_JSON_DEC(benchmark::State& state) {
  //Test R2BindingRequest, a more complex example
  capnp::JsonCodec json;
  capnp::MallocMessageBuilder responseMessage;
  json.handleByAnnotation<workerd::api::public_beta::R2BindingRequest>();
  kj::StringPtr dummy = "{\"version\":1,\"method\":\"completeMultipartUpload\",\"object\":\"multipart_object_name4\",\"uploadId\":\"uploadId\",\"parts\":[{\"etag\":\"1234\",\"part\":1},{\"etag\":\"56789\",\"part\":2}]}"_kj;

  for (auto _ : state) {
    auto responseBuilder = responseMessage.initRoot<workerd::api::public_beta::R2BindingRequest>();
    json.decode(dummy, responseBuilder);
  }
}

// Register the function as a benchmark – we link benchmark_main so there's no need for a main
// function.
WD_BENCHMARK(Test_JSON_ENC);
WD_BENCHMARK(Test_JSON_DEC);

// Benchmarking with the same structure as KJ_TEST is also supported.
WD_TEST_OR_BENCH("Test_JSON_ENC2") {
  capnp::JsonCodec json;
  // Perform setup here

    // This code gets timed
    KJ_EXPECT(json.encode(capnp::VOID) == "null");
    KJ_EXPECT(json.encode(true) == "true");
    KJ_EXPECT(json.encode(false) == "false");
    KJ_EXPECT(json.encode(123) == "123");
    KJ_EXPECT(json.encode(-5.5) == "-5.5");
    KJ_EXPECT(json.encode(capnp::Text::Reader("foo")) == "\"foo\"");
    KJ_EXPECT(json.encode(capnp::Text::Reader("ab\"cd\\ef\x03")) == "\"ab\\\"cd\\\\ef\\u0003\"");
    // TODO
    //KJ_EXPECT(json.encode(capnproto_test::capnp::test::TestEnum::CORGE) == "\"corge\"");

    json.setPrettyPrint(false);
    kj::byte bytes[] = {12, 34, 56};
    KJ_EXPECT(json.encode(capnp::Data::Reader(bytes, 3)) == "[12,34,56]");

    json.setPrettyPrint(true);
    KJ_EXPECT(json.encode(capnp::Data::Reader(bytes, 3)) == "[12, 34, 56]");
}
