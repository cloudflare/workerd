// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Decoding the R2 binding's JSON (src/workerd/api/r2-api.capnp): capnp's JsonCodec, as
// r2-bucket.c++ uses it, against serde_json with the types capnpc-rust-serde generates from the
// same schema (//src/workerd/api:r2-api-serde, wrapped by r2-json-serde/).
//
// Each document of r2-json-serde/fixtures.rs gets three benchmarks:
//
//   <fixture>/capnp            JsonCodec::decode() into a new message, with one codec
//   <fixture>/capnp_new_codec  the same with a new codec per message, as r2-bucket.c++ does
//   <fixture>/serde            capnp_serde::from_slice() into a new Box<T>
//
// Every iteration also destroys what it decoded. bytes_per_second is of JSON input.
//
//   bazel run --config=benchmark //src/workerd/tests:bench-r2-json
//   bazel run --config=benchmark //src/workerd/tests:bench-r2-json -- --benchmark_filter=/serde

#include <workerd/api/r2-api.capnp.h>
#include <workerd/tests/bench-tools.h>
#include <workerd/tests/r2-json-serde/ffi.rs.h>

#include <benchmark/benchmark.h>
#include <kj-rs/convert.h>

#include <capnp/compat/json.h>
#include <capnp/message.h>
#include <kj/debug.h>
#include <kj/string.h>

#include <cstdlib>

namespace workerd {
namespace {

namespace r2 = api::public_beta;
namespace serde = tests::r2_serde;
using serde::Fixture;

kj::ArrayPtr<const char> fixtureJson(Fixture fixture) {
  auto json = serde::fixture(fixture);
  return kj::ArrayPtr<const char>(json.data(), json.size());
}

// Set by the smoke test, which runs each case once.
bool smokeTesting() {
  return std::getenv("R2_JSON_BENCH_SMOKE") != nullptr;
}

template <typename Run>
void finish(benchmark::State& state, kj::ArrayPtr<const char> json, Run& run) {
  state.SetBytesProcessed(static_cast<int64_t>(state.iterations() * json.size()));
  // Allocation stats take another 129 runs.
  if (!smokeTesting()) bench::recordAllocationStats(state, run);
}

template <typename T>
using Check = void (*)(typename T::Reader);

// Decodes the fixture once and checks the result, so that the benchmark measures a decode that
// succeeds.
template <typename T>
void checkCapnp(const capnp::JsonCodec& codec, kj::ArrayPtr<const char> json, Check<T> check) {
  capnp::MallocMessageBuilder message;
  auto root = message.initRoot<T>();
  codec.decode(json, root);
  check(root.asReader());
}

template <typename T>
void capnpDecode(benchmark::State& state, Fixture fixture, Check<T> check) {
  auto json = fixtureJson(fixture);
  capnp::JsonCodec codec;
  codec.handleByAnnotation<T>();
  checkCapnp<T>(codec, json, check);

  auto run = [&]() {
    capnp::MallocMessageBuilder message;
    auto root = message.initRoot<T>();
    codec.decode(json, root);
    benchmark::DoNotOptimize(root);
  };
  for (auto _: state) {
    run();
  }
  finish(state, json, run);
}

template <typename T>
void capnpNewCodecDecode(benchmark::State& state, Fixture fixture, Check<T> check) {
  auto json = fixtureJson(fixture);
  auto run = [&]() {
    capnp::MallocMessageBuilder message;
    capnp::JsonCodec codec;
    codec.handleByAnnotation<T>();
    auto root = message.initRoot<T>();
    codec.decode(json, root);
    benchmark::DoNotOptimize(root);
  };
  {
    capnp::JsonCodec codec;
    codec.handleByAnnotation<T>();
    checkCapnp<T>(codec, json, check);
  }
  for (auto _: state) {
    run();
  }
  finish(state, json, run);
}

// `parse` throws if the fixture does not parse. r2-json-serde's tests check what it parses.
template <typename Parse>
void serdeDecode(benchmark::State& state, Fixture fixture, Parse parse) {
  auto json = fixtureJson(fixture);
  auto bytes = json.asBytes().as<kj_rs::Rust>();
  auto run = [&]() {
    auto value = parse(bytes);
    benchmark::DoNotOptimize(value);
  };
  for (auto _: state) {
    run();
  }
  finish(state, json, run);
}

template <typename T, typename Parse>
void registerFixture(kj::StringPtr name, Fixture fixture, Check<T> check, Parse parse) {
  benchmark::RegisterBenchmark(kj::str(name, "/capnp").cStr(), [=](benchmark::State& state) {
    capnpDecode<T>(state, fixture, check);
  })->Unit(benchmark::kMicrosecond);
  benchmark::RegisterBenchmark(
      kj::str(name, "/capnp_new_codec").cStr(), [=](benchmark::State& state) {
    capnpNewCodecDecode<T>(state, fixture, check);
  })->Unit(benchmark::kMicrosecond);
  benchmark::RegisterBenchmark(kj::str(name, "/serde").cStr(), [=](benchmark::State& state) {
    serdeDecode(state, fixture, parse);
  })->Unit(benchmark::kMicrosecond);
}

// Object counts of the generated fixtures (the constants of r2-json-serde/fixtures.rs).
constexpr uint OBJECTS_IN_SMALL_LIST = 10;
constexpr uint OBJECTS_IN_LARGE_LIST = 1000;
constexpr uint OBJECTS_IN_DELETE = 100;
constexpr uint PARTS_IN_COMPLETE = 100;

// The default of r2-api.capnp's UInt64 fields.
constexpr uint64_t UNSET_UINT64 = kj::maxValue;

void checkListResponse(r2::R2ListResponse::Reader list, uint objects) {
  KJ_ASSERT(list.getObjects().size() == objects);
  auto last = list.getObjects()[objects - 1];
  auto index = kj::str(objects - 1);
  KJ_ASSERT(last.getName() ==
      kj::str("photos/2024/07/IMG_", kj::repeat('0', 4 - index.size()), index, ".jpg"));
  KJ_ASSERT(last.getChecksums().getMd5().size() == 16);
  KJ_ASSERT(last.getHttpFields().getContentType() == "image/jpeg");
  KJ_ASSERT(list.getTruncated());
  KJ_ASSERT(list.getDelimitedPrefixes().size() == 2);
}

const bool registered = [] {
  // Responses, which r2-bucket.c++ and r2-multipart.c++ decode.
  registerFixture<r2::R2HeadResponse>(
      "HeadResponseMinimal", Fixture::HeadResponseMinimal, [](r2::R2HeadResponse::Reader head) {
    KJ_ASSERT(head.getName() == "index.html");
    KJ_ASSERT(head.getSize() == 5123);
    KJ_ASSERT(head.getUploadedMillisecondsSinceEpoch() == 1720000000000);
    KJ_ASSERT(head.getHttpFields().getContentType() == "text/html; charset=utf-8");
    KJ_ASSERT(head.getHttpFields().getCacheExpiry() == UNSET_UINT64);
    KJ_ASSERT(head.getChecksums().getMd5().size() == 16);
    KJ_ASSERT(!head.hasRange());
  }, serde::parse_head_response);
  registerFixture<r2::R2HeadResponse>(
      "HeadResponseFull", Fixture::HeadResponseFull, [](r2::R2HeadResponse::Reader head) {
    KJ_ASSERT(
        head.getHttpFields().getContentDisposition() == "attachment; filename=\"IMG_0001.jpg\"");
    KJ_ASSERT(head.getHttpFields().getCacheExpiry() == 1751536000000);
    KJ_ASSERT(head.getCustomFields().size() == 4);
    KJ_ASSERT(head.getCustomFields()[3].getV() == "Summer \u2600 2024");
    KJ_ASSERT(head.getRange().getLength() == 1048576);
    KJ_ASSERT(head.getRange().getSuffix() == UNSET_UINT64);
    KJ_ASSERT(head.getChecksums().getSha512().size() == 64);
    KJ_ASSERT(head.getSsec().getAlgorithm() == "AES256");
  }, serde::parse_head_response);
  registerFixture<r2::R2ListResponse>("ListResponseSmall", Fixture::ListResponseSmall,
      [](r2::R2ListResponse::Reader list) { checkListResponse(list, OBJECTS_IN_SMALL_LIST); },
      serde::parse_list_response);
  registerFixture<r2::R2ListResponse>("ListResponseLarge", Fixture::ListResponseLarge,
      [](r2::R2ListResponse::Reader list) { checkListResponse(list, OBJECTS_IN_LARGE_LIST); },
      serde::parse_list_response);
  registerFixture<r2::R2CreateMultipartUploadResponse>("CreateMultipartUploadResponse",
      Fixture::CreateMultipartUploadResponse,
      [](r2::R2CreateMultipartUploadResponse::Reader create) {
    KJ_ASSERT(create.getUploadId().startsWith("AJ9x"));
  }, serde::parse_create_multipart_upload_response);
  registerFixture<r2::R2UploadPartResponse>("UploadPartResponse", Fixture::UploadPartResponse,
      [](r2::R2UploadPartResponse::Reader part) { KJ_ASSERT(part.getEtag().size() == 32); },
      serde::parse_upload_part_response);
  registerFixture<r2::R2ErrorResponse>(
      "ErrorResponse", Fixture::ErrorResponse, [](r2::R2ErrorResponse::Reader error) {
    KJ_ASSERT(error.getV4code() == 10007);
    KJ_ASSERT(error.getMessage() == "The specified key does not exist.");
  }, serde::parse_error_response);

  // Requests, which r2-bucket.c++ and r2-multipart.c++ encode.
  registerFixture<r2::R2BindingRequest>(
      "GetRequest", Fixture::GetRequest, [](r2::R2BindingRequest::Reader request) {
    KJ_ASSERT(request.getVersion() == 1);
    auto get = request.getPayload().getGet();
    KJ_ASSERT(get.getRange().getLength() == 1048576);
    KJ_ASSERT(get.getOnlyIf().getEtagMatches().size() == 1);
    KJ_ASSERT(get.getOnlyIf().getEtagMatches()[0].getType().isStrong());
    KJ_ASSERT(get.getOnlyIf().getUploadedAfter() == 1719999999000);
  }, serde::parse_binding_request);
  registerFixture<r2::R2BindingRequest>(
      "PutRequest", Fixture::PutRequest, [](r2::R2BindingRequest::Reader request) {
    auto put = request.getPayload().getPut();
    KJ_ASSERT(put.getCustomFields().size() == 2);
    KJ_ASSERT(put.getMd5().size() == 16);
    KJ_ASSERT(put.getSha256().size() == 32);
    KJ_ASSERT(!put.hasSha1());
  }, serde::parse_binding_request);
  registerFixture<r2::R2BindingRequest>(
      "ListRequest", Fixture::ListRequest, [](r2::R2BindingRequest::Reader request) {
    auto list = request.getPayload().getList();
    KJ_ASSERT(list.getLimit() == 1000);
    KJ_ASSERT(list.getDelimiter() == "/");
    KJ_ASSERT(list.getInclude().size() == 2);
    KJ_ASSERT(list.getNewRuntime());
  }, serde::parse_binding_request);
  registerFixture<r2::R2BindingRequest>(
      "DeleteRequest", Fixture::DeleteRequest, [](r2::R2BindingRequest::Reader request) {
    KJ_ASSERT(request.getPayload().getDelete().getObjects().size() == OBJECTS_IN_DELETE);
  }, serde::parse_binding_request);
  registerFixture<r2::R2BindingRequest>("CompleteMultipartUploadRequest",
      Fixture::CompleteMultipartUploadRequest, [](r2::R2BindingRequest::Reader request) {
    auto parts = request.getPayload().getCompleteMultipartUpload().getParts();
    KJ_ASSERT(parts.size() == PARTS_IN_COMPLETE);
    KJ_ASSERT(parts[PARTS_IN_COMPLETE - 1].getPart() == PARTS_IN_COMPLETE);
  }, serde::parse_binding_request);
  return true;
}();

}  // namespace
}  // namespace workerd
