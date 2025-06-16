// Copyright (c) 2017-2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "r2-bucket.h"

#include <workerd/api/http.h>
#include <workerd/api/r2-api.capnp.h>
#include <workerd/api/streams/readable.h>
#include <workerd/io/trace.h>
#include <workerd/jsg/jsg.h>

#include <kj/encoding.h>

#include <regex>

namespace workerd::api::public_beta {

// Forward declaration for buildEtagsString (defined in r2-bucket.c++)
kj::String buildEtagsString(kj::ArrayPtr<R2Bucket::Etag> etagArray);

// Convert a kj::Date to ISO string format for tracing
inline kj::String toISOString(jsg::Lock& js, kj::Date date) {
  return js.date(date).toISOString(js);
}

// Shared helper for adding R2 response span tags
inline void addR2ResponseSpanTags(TraceContext& traceContext, R2Result& r2Result) {
  traceContext.userSpan.setTag("cloudflare.r2.response.success"_kjc, r2Result.success());
  KJ_IF_SOME(e, r2Result.getR2ErrorMessage()) {
    traceContext.userSpan.setTag("error.type"_kjc, e.asPtr());
    traceContext.userSpan.setTag("cloudflare.r2.error.message"_kjc, e.asPtr());
  }
  KJ_IF_SOME(v4, r2Result.v4ErrorCode()) {
    traceContext.userSpan.setTag("cloudflare.r2.error.code"_kjc, static_cast<int64_t>(v4));
  }
}

// Utility function for initOnlyIf
inline void addEtagsToBuilder(
    capnp::List<R2Etag>::Builder etagListBuilder, kj::ArrayPtr<R2Bucket::Etag> etagArray) {
  R2Bucket::Etag* currentEtag = etagArray.begin();
  for (unsigned int i = 0; i < etagArray.size(); i++) {
    KJ_SWITCH_ONEOF(*currentEtag) {
      KJ_CASE_ONEOF(e, R2Bucket::WildcardEtag) {
        etagListBuilder[i].initType().setWildcard();
      }
      KJ_CASE_ONEOF(e, R2Bucket::StrongEtag) {
        etagListBuilder[i].initType().setStrong();
        etagListBuilder[i].setValue(e.value);
      }
      KJ_CASE_ONEOF(e, R2Bucket::WeakEtag) {
        etagListBuilder[i].initType().setWeak();
        etagListBuilder[i].setValue(e.value);
      }
    }
    currentEtag = std::next(currentEtag);
  }
}

inline bool isWholeNumber(double x) {
  double intpart;
  return modf(x, &intpart) == 0;
}

template <typename Builder, typename Options>
void initRange(jsg::Lock& js, Builder& builder, Options& o) {
  KJ_IF_SOME(range, o.range) {
    KJ_SWITCH_ONEOF(range) {
      KJ_CASE_ONEOF(r, R2Bucket::Range) {
        auto rangeBuilder = builder.initRange();
        KJ_IF_SOME(offset, r.offset) {
          JSG_REQUIRE(offset >= 0, RangeError, "Invalid range. Starting offset (", offset,
              ") must be greater than or equal to 0.");
          JSG_REQUIRE(isWholeNumber(offset), RangeError, "Invalid range. Starting offset (", offset,
              ") must be an integer, not floating point.");
          rangeBuilder.setOffset(static_cast<uint64_t>(offset));
        }

        KJ_IF_SOME(length, r.length) {
          JSG_REQUIRE(length >= 0, RangeError, "Invalid range. Length (", length,
              ") must be greater than or equal to 0.");
          JSG_REQUIRE(isWholeNumber(length), RangeError, "Invalid range. Length (", length,
              ") must be an integer, not floating point.");

          rangeBuilder.setLength(static_cast<uint64_t>(length));
        }
        KJ_IF_SOME(suffix, r.suffix) {
          JSG_REQUIRE(r.offset == kj::none, TypeError, "Suffix is incompatible with offset.");
          JSG_REQUIRE(r.length == kj::none, TypeError, "Suffix is incompatible with length.");

          JSG_REQUIRE(suffix >= 0, RangeError, "Invalid suffix. Suffix (", suffix,
              ") must be greater than or equal to 0.");
          JSG_REQUIRE(isWholeNumber(suffix), RangeError, "Invalid range. Suffix (", suffix,
              ") must be an integer, not floating point.");

          rangeBuilder.setSuffix(static_cast<uint64_t>(suffix));
        }
      }
      KJ_CASE_ONEOF(h, jsg::Ref<Headers>) {
        KJ_IF_SOME(e, h->getNoChecks(js, "range"_kj)) {
          builder.setRangeHeader(kj::str(e));
        }
      }
    }
  }
}
inline const std::regex hexPattern("^[0-9a-f]+$");
template <typename Builder, typename Options>
void initSsec(jsg::Lock& js, Builder& builder, Options& o) {
  KJ_IF_SOME(rawSsecKey, o.ssecKey) {
    auto ssecBuilder = builder.initSsec();
    // SSE-C key must be 32 bytes.
    // When provided as a hex string, it must be 64 characters long.
    // When provided as a byte array, it must be 32 bytes long.
    KJ_SWITCH_ONEOF(rawSsecKey) {
      KJ_CASE_ONEOF(keyString, kj::String) {
        JSG_REQUIRE(std::regex_match(keyString.begin(), keyString.end(), hexPattern), Error,
            "SSE-C Key has invalid format");
        JSG_REQUIRE(keyString.size() == 64, Error, "SSE-C Key must be 32 bytes in length");
        ssecBuilder.setKey(keyString);
      }
      KJ_CASE_ONEOF(keyBuff, kj::Array<byte>) {
        JSG_REQUIRE(keyBuff.size() == 32, Error, "SSE-C Key must be 32 bytes in length");
        ssecBuilder.setKey(kj::encodeHex(keyBuff));
      }
    }
  }
}
}  // namespace workerd::api::public_beta
