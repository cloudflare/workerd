// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/api/r2-api.capnp.h>
#include <workerd/api/r2_api_serde_json_ffi.rs.h>
#include <workerd/util/autogate.h>

#include <kj-rs/convert.h>

#include <capnp/compat/json.h>
#include <capnp/message.h>
#include <capnp/schema.h>
#include <kj/debug.h>
#include <kj/exception.h>

namespace workerd::api::public_beta {

// Each overload reports the first differing field as a path relative to the root. Paths can
// be logged without exposing values from R2 responses.
kj::Maybe<kj::String> findR2JsonDifference(
    R2HeadResponse::Reader capnp, const serde::R2HeadResponse& serde);
kj::Maybe<kj::String> findR2JsonDifference(
    R2ListResponse::Reader capnp, const serde::R2ListResponse& serde);
kj::Maybe<kj::String> findR2JsonDifference(R2CreateMultipartUploadResponse::Reader capnp,
    const serde::R2CreateMultipartUploadResponse& serde);
kj::Maybe<kj::String> findR2JsonDifference(
    R2UploadPartResponse::Reader capnp, const serde::R2UploadPartResponse& serde);
kj::Maybe<kj::String> findR2JsonDifference(
    R2ErrorResponse::Reader capnp, const serde::R2ErrorResponse& serde);

// Calls consume with the authoritative reader while its message is alive. Each decoder is
// attempted independently so that a rejection by either one can be detected in both gate states.
// The callback must return an owned result, since either reader can borrow from local storage.
template <typename Capnp, typename Serde, typename Consume>
auto withR2JsonResponse(kj::ArrayPtr<const char> payload,
    ::rust::Box<Serde> (*parseSerde)(::rust::Slice<const uint8_t>),
    Consume&& consume) {
  capnp::MallocMessageBuilder message;
  typename Capnp::Reader capnpResponse;
  auto capnpError = kj::runCatchingExceptions([&] {
    capnp::JsonCodec codec;
    codec.handleByAnnotation<Capnp>();
    auto root = message.initRoot<Capnp>();
    codec.decode(payload, root);
    capnpResponse = root.asReader();
  });
  kj::Maybe<::rust::Box<Serde>> serdeResponse;
  // Serde's error description can contain values from the response.
  const bool serdeRejected = kj::runCatchingExceptions([&] {
    serdeResponse = parseSerde(payload.asBytes().as<kj_rs::Rust>());
  }) != kj::none;

  const auto responseType = capnp::Schema::from<Capnp>().getShortDisplayName();
  // Comparison diagnostics are best-effort. A failed log write must leave the decode outcome intact.
  (void)kj::runCatchingExceptions([&] {
    if ((capnpError != kj::none) != serdeRejected) {
      auto rejectedBy = serdeRejected ? "capnp-serde-json" : "JsonCodec";
      KJ_LOG(ERROR, "R2 JSON decoder disagreement", responseType, rejectedBy);
    } else if (!serdeRejected) {
      kj::Maybe<kj::String> difference;
      auto comparisonError = kj::runCatchingExceptions([&] {
        difference = findR2JsonDifference(capnpResponse, *KJ_ASSERT_NONNULL(serdeResponse));
      });
      if (comparisonError != kj::none) {
        KJ_LOG(ERROR, "R2 JSON response comparison failed", responseType);
      } else KJ_IF_SOME(field, difference) {
        KJ_LOG(ERROR, "R2 JSON response mismatch", responseType, field);
      }
    }
  });

  if (util::Autogate::isEnabled(util::AutogateKey::R2_SERDE_JSON)) {
    if (serdeRejected) {
      kj::throwFatalException(
          KJ_EXCEPTION(FAILED, "Failed to decode R2 JSON response with Serde", responseType));
    }
    return consume(*KJ_ASSERT_NONNULL(serdeResponse));
  }
  KJ_IF_SOME(error, capnpError) {
    kj::throwFatalException(kj::mv(error));
  }
  return consume(capnpResponse);
}

}  // namespace workerd::api::public_beta
