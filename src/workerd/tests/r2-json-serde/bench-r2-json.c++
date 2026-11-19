// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Decoding the R2 binding's JSON (src/workerd/api/r2-api.capnp): capnp's JsonCodec, as
// r2-bucket.c++ uses it, against serde_json with the types capnpc-rust-serde-json generates from
// the same schema (//src/workerd/api:r2-api-serde-json).
//
// Each document of r2-json-serde/fixtures.rs gets four benchmarks:
//
//   <fixture>/capnp            JsonCodec::decode() into a new message, with one codec
//   <fixture>/capnp_new_codec  the same with a new codec per message, as r2-bucket.c++ does
//   <fixture>/serde            capnp_serde_json::from_slice() into a new Box<T>, through
//                              r2-json-serde's bridge
//   <fixture>/ffi              serde::parseFoo() of r2-api-serde-json's cxx bridge, through
//                              which C++ code decodes into the generated types
//
// Every iteration also destroys what it decoded. bytes_per_second is of JSON input. Before it
// measures, each benchmark but serde checks the fields of one decoded document, through capnp's
// readers or the bridge's getters; r2-json-serde's Rust tests check what serde decodes.
//
//   bazel run --config=benchmark //src/workerd/tests/r2-json-serde:bench-r2-json
//   bazel run --config=benchmark //src/workerd/tests/r2-json-serde:bench-r2-json -- --benchmark_filter=/serde

#include <workerd/api/r2-api.capnp.h>
#include <workerd/api/r2_api_serde_json_ffi.rs.h>
#include <workerd/tests/bench-tools.h>
#include <workerd/tests/r2-json-serde/ffi.rs.h>

#include <benchmark/benchmark.h>
#include <kj-rs/convert.h>

#include <capnp/compat/json.h>
#include <capnp/message.h>
#include <kj/debug.h>
#include <kj/string.h>

namespace workerd {
namespace {

namespace r2 = api::public_beta;
namespace serde = tests::r2_serde;
namespace ffi = r2::serde;
using serde::Fixture;

kj::ArrayPtr<const char> fixtureJson(Fixture fixture) {
  auto json = serde::fixture(fixture);
  return kj::ArrayPtr<const char>(json.data(), json.size());
}

template <typename Run>
void finish(benchmark::State& state, kj::ArrayPtr<const char> json, Run& run) {
  state.SetBytesProcessed(static_cast<int64_t>(state.iterations() * json.size()));
  bench::recordAllocationStats(state, run);
}

// A Text field's characters, from capnp's readers and from the bridge's getters, so that a check
// reads both.
kj::ArrayPtr<const char> chars(capnp::Text::Reader text) {
  return text.asArray();
}

kj::ArrayPtr<const char> chars(rust::Str text) {
  return kj::from<kj_rs::Rust>(text);
}

bool textIs(auto text, kj::StringPtr expected) {
  return chars(text) == expected.asArray();
}

bool textStartsWith(auto text, kj::StringPtr prefix) {
  return chars(text).startsWith(prefix.asArray());
}

// A check is a generic callable that asserts the fields of a decoded document, given either a
// capnp reader (T::Reader) or a value of the bridge's type for T.

// Decodes the fixture once and checks the result, so that the benchmark measures a decode that
// succeeds.
template <typename T, typename Check>
void checkCapnp(const capnp::JsonCodec& codec, kj::ArrayPtr<const char> json, Check& check) {
  capnp::MallocMessageBuilder message;
  auto root = message.initRoot<T>();
  codec.decode(json, root);
  check(root.asReader());
}

template <typename T, typename Check>
void capnpDecode(benchmark::State& state, Fixture fixture, Check check) {
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

template <typename T, typename Check>
void capnpNewCodecDecode(benchmark::State& state, Fixture fixture, Check check) {
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

// `parse`, a function of a cxx bridge, throws if the fixture does not parse. `check` is given what
// it parses once, before the benchmark measures.
template <typename Parse, typename Check>
void rustDecode(benchmark::State& state, Fixture fixture, Parse parse, Check check) {
  auto json = fixtureJson(fixture);
  auto bytes = json.asBytes().as<kj_rs::Rust>();
  check(*parse(bytes));
  auto run = [&]() {
    auto value = parse(bytes);
    benchmark::DoNotOptimize(value);
  };
  for (auto _: state) {
    run();
  }
  finish(state, json, run);
}

// `serdeParse` is r2-json-serde's parse function for T, `ffiParse` the generated bridge's. The
// types of r2-json-serde's bridge have no getters, so only JsonCodec's and `ffiParse`'s results
// are checked.
template <typename T, typename Check, typename SerdeParse, typename FfiParse>
void registerFixture(
    kj::StringPtr name, Fixture fixture, Check check, SerdeParse serdeParse, FfiParse ffiParse) {
  benchmark::RegisterBenchmark(kj::str(name, "/capnp").cStr(), [=](benchmark::State& state) {
    capnpDecode<T>(state, fixture, check);
  })->Unit(benchmark::kMicrosecond);
  benchmark::RegisterBenchmark(
      kj::str(name, "/capnp_new_codec").cStr(), [=](benchmark::State& state) {
    capnpNewCodecDecode<T>(state, fixture, check);
  })->Unit(benchmark::kMicrosecond);
  benchmark::RegisterBenchmark(kj::str(name, "/serde").cStr(), [=](benchmark::State& state) {
    rustDecode(state, fixture, serdeParse, [](const auto&) {});
  })->Unit(benchmark::kMicrosecond);
  benchmark::RegisterBenchmark(kj::str(name, "/ffi").cStr(), [=](benchmark::State& state) {
    rustDecode(state, fixture, ffiParse, check);
  })->Unit(benchmark::kMicrosecond);
}

// Object counts of the generated fixtures (the constants of r2-json-serde/fixtures.rs).
constexpr uint32_t OBJECTS_IN_SMALL_LIST = 10;
constexpr uint32_t OBJECTS_IN_LARGE_LIST = 1000;
constexpr uint32_t OBJECTS_IN_DELETE = 100;
constexpr uint32_t PARTS_IN_COMPLETE = 100;

// The default of r2-api.capnp's UInt64 fields.
constexpr uint64_t UNSET_UINT64 = kj::maxValue;

void checkListResponse(const auto& list, uint32_t objects) {
  KJ_ASSERT(list.getObjects().size() == objects);
  auto&& last = list.getObjects()[objects - 1];
  auto index = kj::str(objects - 1);
  KJ_ASSERT(textIs(last.getName(),
      kj::str("photos/2024/07/IMG_", kj::repeat('0', 4 - index.size()), index, ".jpg")));
  KJ_ASSERT(last.getChecksums().getMd5().size() == 16);
  KJ_ASSERT(textIs(last.getHttpFields().getContentType(), "image/jpeg"));
  KJ_ASSERT(list.getTruncated());
  KJ_ASSERT(list.getDelimitedPrefixes().size() == 2);
}

const bool registered = [] {
  // Responses, which r2-bucket.c++ and r2-multipart.c++ decode.
  registerFixture<r2::R2HeadResponse>(
      "HeadResponseMinimal", Fixture::HeadResponseMinimal, [](const auto& head) {
    KJ_ASSERT(textIs(head.getName(), "index.html"));
    KJ_ASSERT(head.getSize() == 5123);
    KJ_ASSERT(head.getUploadedMillisecondsSinceEpoch() == 1720000000000);
    KJ_ASSERT(textIs(head.getHttpFields().getContentType(), "text/html; charset=utf-8"));
    KJ_ASSERT(head.getHttpFields().getCacheExpiry() == UNSET_UINT64);
    KJ_ASSERT(head.getChecksums().getMd5().size() == 16);
    KJ_ASSERT(!head.hasRange());
  }, serde::parse_head_response, ffi::parseR2HeadResponse);
  registerFixture<r2::R2HeadResponse>(
      "HeadResponseFull", Fixture::HeadResponseFull, [](const auto& head) {
    KJ_ASSERT(textIs(
        head.getHttpFields().getContentDisposition(), "attachment; filename=\"IMG_0001.jpg\""));
    KJ_ASSERT(head.getHttpFields().getCacheExpiry() == 1751536000000);
    KJ_ASSERT(head.getCustomFields().size() == 4);
    KJ_ASSERT(textIs(head.getCustomFields()[3].getV(), "Summer \u2600 2024"));
    KJ_ASSERT(head.getRange().getLength() == 1048576);
    KJ_ASSERT(head.getRange().getSuffix() == UNSET_UINT64);
    KJ_ASSERT(head.getChecksums().getSha512().size() == 64);
    KJ_ASSERT(textIs(head.getSsec().getAlgorithm(), "AES256"));
  }, serde::parse_head_response, ffi::parseR2HeadResponse);
  registerFixture<r2::R2ListResponse>("ListResponseSmall", Fixture::ListResponseSmall,
      [](const auto& list) { checkListResponse(list, OBJECTS_IN_SMALL_LIST); },
      serde::parse_list_response, ffi::parseR2ListResponse);
  registerFixture<r2::R2ListResponse>("ListResponseLarge", Fixture::ListResponseLarge,
      [](const auto& list) { checkListResponse(list, OBJECTS_IN_LARGE_LIST); },
      serde::parse_list_response, ffi::parseR2ListResponse);
  registerFixture<r2::R2CreateMultipartUploadResponse>("CreateMultipartUploadResponse",
      Fixture::CreateMultipartUploadResponse, [](const auto& create) {
    KJ_ASSERT(textStartsWith(create.getUploadId(), "AJ9x"));
  }, serde::parse_create_multipart_upload_response, ffi::parseR2CreateMultipartUploadResponse);
  registerFixture<r2::R2UploadPartResponse>("UploadPartResponse", Fixture::UploadPartResponse,
      [](const auto& part) { KJ_ASSERT(part.getEtag().size() == 32); },
      serde::parse_upload_part_response, ffi::parseR2UploadPartResponse);
  registerFixture<r2::R2ErrorResponse>(
      "ErrorResponse", Fixture::ErrorResponse, [](const auto& error) {
    KJ_ASSERT(error.getV4code() == 10007);
    KJ_ASSERT(textIs(error.getMessage(), "The specified key does not exist."));
  }, serde::parse_error_response, ffi::parseR2ErrorResponse);

  // Requests, which r2-bucket.c++ and r2-multipart.c++ encode.
  registerFixture<r2::R2BindingRequest>("GetRequest", Fixture::GetRequest, [](const auto& request) {
    KJ_ASSERT(request.getVersion() == 1);
    auto&& get = request.getPayload().getGet();
    KJ_ASSERT(get.getRange().getLength() == 1048576);
    KJ_ASSERT(get.getOnlyIf().getEtagMatches().size() == 1);
    KJ_ASSERT(get.getOnlyIf().getEtagMatches()[0].getType().isStrong());
    KJ_ASSERT(get.getOnlyIf().getUploadedAfter() == 1719999999000);
  }, serde::parse_binding_request, ffi::parseR2BindingRequest);
  registerFixture<r2::R2BindingRequest>("PutRequest", Fixture::PutRequest, [](const auto& request) {
    auto&& put = request.getPayload().getPut();
    KJ_ASSERT(put.getCustomFields().size() == 2);
    KJ_ASSERT(put.getMd5().size() == 16);
    KJ_ASSERT(put.getSha256().size() == 32);
    KJ_ASSERT(!put.hasSha1());
  }, serde::parse_binding_request, ffi::parseR2BindingRequest);
  registerFixture<r2::R2BindingRequest>(
      "ListRequest", Fixture::ListRequest, [](const auto& request) {
    auto&& list = request.getPayload().getList();
    KJ_ASSERT(list.getLimit() == 1000);
    KJ_ASSERT(textIs(list.getDelimiter(), "/"));
    KJ_ASSERT(list.getInclude().size() == 2);
    KJ_ASSERT(list.getNewRuntime());
  }, serde::parse_binding_request, ffi::parseR2BindingRequest);
  registerFixture<r2::R2BindingRequest>(
      "DeleteRequest", Fixture::DeleteRequest, [](const auto& request) {
    KJ_ASSERT(request.getPayload().getDelete().getObjects().size() == OBJECTS_IN_DELETE);
  }, serde::parse_binding_request, ffi::parseR2BindingRequest);
  registerFixture<r2::R2BindingRequest>("CompleteMultipartUploadRequest",
      Fixture::CompleteMultipartUploadRequest, [](const auto& request) {
    auto&& parts = request.getPayload().getCompleteMultipartUpload().getParts();
    KJ_ASSERT(parts.size() == PARTS_IN_COMPLETE);
    KJ_ASSERT(parts[PARTS_IN_COMPLETE - 1].getPart() == PARTS_IN_COMPLETE);
  }, serde::parse_binding_request, ffi::parseR2BindingRequest);
  return true;
}();

}  // namespace
}  // namespace workerd
