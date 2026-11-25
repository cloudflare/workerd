// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "r2-json.h"

namespace workerd::api::public_beta {
namespace {

// Compare by length and bytes so that embedded NULs in metadata are included.
kj::Maybe<kj::String> findR2JsonDifference(capnp::Text::Reader cpp, ::rust::Str rust) {
  if (cpp.asArray() != kj::from<kj_rs::Rust>(rust)) return kj::str("");
  return kj::none;
}

// Compare decoded bytes so equivalent JSON encodings of a checksum compare equal.
kj::Maybe<kj::String> findR2JsonDifference(
    capnp::Data::Reader cpp, ::rust::Slice<const uint8_t> rust) {
  if (cpp != kj::from<kj_rs::Rust>(rust)) return kj::str("");
  return kj::none;
}

// Cap'n Proto readers return defaults for null pointers, so compare presence before values.
#define R2_COMPARE_POINTER(field)                                                                  \
  if (auto present = cpp.has##field(); present != rust.has##field()) {                             \
    return kj::str("." #field " (presence)");                                                      \
  } else if (present) {                                                                            \
    KJ_IF_SOME(diff, findR2JsonDifference(cpp.get##field(), rust.get##field())) {                  \
      return kj::str("." #field, diff);                                                            \
    }                                                                                              \
  }

#define R2_COMPARE_VALUE(field)                                                                    \
  if (cpp.get##field() != rust.get##field()) return kj::str("." #field);

// Preserve the distinction between missing and empty keys and values in custom metadata.
kj::Maybe<kj::String> findR2JsonDifference(Record::Reader cpp, const serde::Record& rust) {
  R2_COMPARE_POINTER(K);
  R2_COMPARE_POINTER(V);
  return kj::none;
}

// Compare both the presence of each header and the cache expiry's decoded timestamp.
kj::Maybe<kj::String> findR2JsonDifference(
    R2HttpFields::Reader cpp, const serde::R2HttpFields& rust) {
  R2_COMPARE_POINTER(ContentType);
  R2_COMPARE_POINTER(ContentLanguage);
  R2_COMPARE_POINTER(ContentDisposition);
  R2_COMPARE_POINTER(ContentEncoding);
  R2_COMPARE_POINTER(CacheControl);
  R2_COMPARE_VALUE(CacheExpiry);
  return kj::none;
}

// Include suffix even though the JS response only uses offset and length.
kj::Maybe<kj::String> findR2JsonDifference(R2Range::Reader cpp, const serde::R2Range& rust) {
  R2_COMPARE_VALUE(Offset);
  R2_COMPARE_VALUE(Length);
  R2_COMPARE_VALUE(Suffix);
  return kj::none;
}

// Each checksum's presence and bytes are independent of the other algorithms.
kj::Maybe<kj::String> findR2JsonDifference(
    R2Checksums::Reader cpp, const serde::R2Checksums& rust) {
  R2_COMPARE_POINTER(Md5);
  R2_COMPARE_POINTER(Sha1);
  R2_COMPARE_POINTER(Sha256);
  R2_COMPARE_POINTER(Sha384);
  R2_COMPARE_POINTER(Sha512);
  return kj::none;
}

// Compare the algorithm along with the digest, even where JS only receives the digest.
kj::Maybe<kj::String> findR2JsonDifference(
    R2SSECResponse::Reader cpp, const serde::R2SSECResponse& rust) {
  R2_COMPARE_POINTER(Algorithm);
  R2_COMPARE_POINTER(KeyMd5);
  return kj::none;
}

// Compare in wire order and identify the first differing element by its index.
template <typename CppList, typename RustItem>
kj::Maybe<kj::String> findR2JsonDifference(CppList cpp, ::rust::Slice<const RustItem> rust) {
  if (cpp.size() != rust.size()) return kj::str(" (length)");
  for (uint i = 0; i < cpp.size(); ++i) {
    KJ_IF_SOME(diff, findR2JsonDifference(cpp[i], rust[i])) {
      return kj::str("[", i, "]", diff);
    }
  }
  return kj::none;
}

}  // namespace

kj::Maybe<kj::String> findR2JsonDifference(
    R2HeadResponse::Reader cpp, const serde::R2HeadResponse& rust) {
  R2_COMPARE_POINTER(Name);
  R2_COMPARE_POINTER(Version);
  R2_COMPARE_VALUE(Size);
  R2_COMPARE_POINTER(Etag);
  R2_COMPARE_VALUE(UploadedMillisecondsSinceEpoch);
  R2_COMPARE_POINTER(HttpFields);
  R2_COMPARE_POINTER(CustomFields);
  R2_COMPARE_POINTER(Range);
  R2_COMPARE_POINTER(Checksums);
  R2_COMPARE_POINTER(StorageClass);
  R2_COMPARE_POINTER(Ssec);
  return kj::none;
}

kj::Maybe<kj::String> findR2JsonDifference(
    R2ListResponse::Reader cpp, const serde::R2ListResponse& rust) {
  R2_COMPARE_POINTER(Objects);
  R2_COMPARE_VALUE(Truncated);
  R2_COMPARE_POINTER(Cursor);
  R2_COMPARE_POINTER(DelimitedPrefixes);
  return kj::none;
}

kj::Maybe<kj::String> findR2JsonDifference(R2CreateMultipartUploadResponse::Reader cpp,
    const serde::R2CreateMultipartUploadResponse& rust) {
  R2_COMPARE_POINTER(UploadId);
  R2_COMPARE_POINTER(Ssec);
  return kj::none;
}

kj::Maybe<kj::String> findR2JsonDifference(
    R2UploadPartResponse::Reader cpp, const serde::R2UploadPartResponse& rust) {
  R2_COMPARE_POINTER(Etag);
  return kj::none;
}

kj::Maybe<kj::String> findR2JsonDifference(
    R2ErrorResponse::Reader cpp, const serde::R2ErrorResponse& rust) {
  R2_COMPARE_VALUE(Version);
  R2_COMPARE_VALUE(V4code);
  R2_COMPARE_POINTER(Message);
  return kj::none;
}

#undef R2_COMPARE_POINTER
#undef R2_COMPARE_VALUE

}  // namespace workerd::api::public_beta
