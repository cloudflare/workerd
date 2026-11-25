// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "r2-json.h"

#include <kj/test.h>

namespace workerd::api::public_beta {
namespace {

class FailingR2DiagnosticLogger final: public kj::ExceptionCallback {
 public:
  uint attempts = 0;

 private:
  // Simulate a failed diagnostic write while leaving test framework logging available.
  void logMessage(kj::LogSeverity severity,
      const char* file,
      int line,
      int contextDepth,
      kj::String&& text) override {
    if (text.startsWith("R2 JSON "_kj)) {
      ++attempts;
      kj::throwFatalException(KJ_EXCEPTION(FAILED, "diagnostic output unavailable"));
    }
    kj::ExceptionCallback::logMessage(severity, file, line, contextDepth, kj::mv(text));
  }
};

KJ_TEST("R2 JSON logs nested field differences and returns the selected decoder's value") {
  // JsonCodec reads JSON numbers as doubles. Serde preserves all the bits of an integer.
  auto decodeSize = [] {
    return withR2JsonResponse<R2ListResponse>(R"({"objects":[{"size":9007199254740993}]})"_kj,
        serde::parseR2ListResponse,
        [](const auto& response) { return response.getObjects()[0].getSize(); });
  };

  util::Autogate::initAutogateNamesForTest({}, util::IgnoreAllAutogatesEnv::YES);
  KJ_DEFER(util::Autogate::deinitAutogate());
  {
    KJ_EXPECT_LOG(ERROR, ".Objects[0].Size");
    KJ_EXPECT(decodeSize() == 9007199254740992ull);
  }
  util::Autogate::initAutogateForTest({util::AutogateKey::R2_SERDE_JSON});
  {
    KJ_EXPECT_LOG(ERROR, ".Objects[0].Size");
    KJ_EXPECT(decodeSize() == 9007199254740993ull);
  }
  {
    FailingR2DiagnosticLogger logger;
    KJ_EXPECT(decodeSize() == 9007199254740993ull);
    KJ_EXPECT(logger.attempts == 1);
  }
}

KJ_TEST("R2 JSON propagates a decoder rejection only when that decoder is authoritative") {
  // JsonCodec takes the last value of a repeated key. Serde rejects the message.
  auto decodeEtag = [] {
    return withR2JsonResponse<R2UploadPartResponse>(R"({"etag":"first","etag":"last"})"_kj,
        serde::parseR2UploadPartResponse,
        [](const auto& response) { return kj::str(response.getEtag()); });
  };

  util::Autogate::initAutogateNamesForTest({}, util::IgnoreAllAutogatesEnv::YES);
  KJ_DEFER(util::Autogate::deinitAutogate());
  {
    KJ_EXPECT_LOG(ERROR, "rejectedBy = capnp-serde-json");
    KJ_EXPECT(decodeEtag() == "last");
  }
  {
    FailingR2DiagnosticLogger logger;
    KJ_EXPECT(decodeEtag() == "last");
    KJ_EXPECT(logger.attempts == 1);
  }
  util::Autogate::initAutogateForTest({util::AutogateKey::R2_SERDE_JSON});
  {
    KJ_EXPECT_LOG(ERROR, "rejectedBy = capnp-serde-json");
    KJ_EXPECT_THROW_MESSAGE("R2UploadPartResponse", decodeEtag());
  }
  {
    FailingR2DiagnosticLogger logger;
    KJ_EXPECT_THROW_MESSAGE("R2UploadPartResponse", decodeEtag());
    KJ_EXPECT(logger.attempts == 1);
  }
}

KJ_TEST("R2 JSON Serde errors redact rejected response values") {
  util::Autogate::initAutogateForTest({util::AutogateKey::R2_SERDE_JSON});
  KJ_DEFER(util::Autogate::deinitAutogate());

  // A string in place of the metadata list causes Serde to quote the value in its error.
  auto error = kj::runCatchingExceptions([] {
    withR2JsonResponse<R2HeadResponse>(R"({"customFields":"private-metadata-value"})"_kj,
        serde::parseR2HeadResponse, [](const auto&) {});
  });
  auto description = KJ_ASSERT_NONNULL(error).getDescription();
  KJ_EXPECT(description.contains("R2HeadResponse"_kj));
  KJ_EXPECT(!description.contains("private-metadata-value"_kj));
}

}  // namespace
}  // namespace workerd::api::public_beta
