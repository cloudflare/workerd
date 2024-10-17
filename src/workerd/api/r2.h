// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "r2-bucket.h"
#include "r2-multipart.h"

namespace workerd::api::public_beta {
#define EW_R2_PUBLIC_BETA_ISOLATE_TYPES                                                            \
  api::R2Error, api::public_beta::R2Bucket, api::public_beta::R2MultipartUpload,                   \
      api::public_beta::R2MultipartUpload::UploadedPart, api::public_beta::R2Bucket::HeadResult,   \
      api::public_beta::R2Bucket::GetResult, api::public_beta::R2Bucket::Range,                    \
      api::public_beta::R2Bucket::Conditional, api::public_beta::R2Bucket::GetOptions,             \
      api::public_beta::R2Bucket::PutOptions, api::public_beta::R2Bucket::MultipartOptions,        \
      api::public_beta::R2Bucket::Checksums, api::public_beta::R2Bucket::StringChecksums,          \
      api::public_beta::R2Bucket::HttpMetadata, api::public_beta::R2Bucket::ListOptions,           \
      api::public_beta::R2Bucket::ListResult,                                                      \
      api::public_beta::R2MultipartUpload::UploadPartOptions
// The list of r2 types that are added to worker.c++'s JSG_DECLARE_ISOLATE_TYPE
}  // namespace workerd::api::public_beta
