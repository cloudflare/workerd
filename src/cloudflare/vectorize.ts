// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

/**
 * A pre-configured list of known models.
 * These can be supplied in place of configuring explicit dimensions.
 */
export enum KnownModel {
  "openapi-text-embedding-ada-002" = "openapi-text-embedding-ada-002",
  "workers-ai/bge-small-en" = "workers-ai/bge-small-en",
  "cohere/embed-multilingual-v2.0" = "cohere/embed-multilingual-v2.0",
}

/**
 * Supported distance metrics for an index.
 * Distance metrics determine how other "similar" vectors are determined.
 */
export enum DistanceMetric {
  EUCLIDEAN = "euclidean",
  COSINE = "cosine",
  DOT_PRODUCT = "dot-product",
}
