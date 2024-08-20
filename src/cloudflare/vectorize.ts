// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

/**
 * A pre-configured list of known models.
 * These can be supplied in place of configuring explicit dimensions.
 */
export enum KnownModel {
  'openai/text-embedding-ada-002' = 'openai/text-embedding-ada-002',
  'cohere/embed-multilingual-v2.0' = 'cohere/embed-multilingual-v2.0',
  '@cf/baai/bge-small-en-v1.5' = '@cf/baai/bge-small-en-v1.5',
  '@cf/baai/bge-base-en-v1.5' = '@cf/baai/bge-base-en-v1.5',
  '@cf/baai/bge-large-en-v1.5' = '@cf/baai/bge-large-en-v1.5',
}

/**
 * Supported distance metrics for an index.
 * Distance metrics determine how other "similar" vectors are determined.
 */
export enum DistanceMetric {
  EUCLIDEAN = 'euclidean',
  COSINE = 'cosine',
  DOT_PRODUCT = 'dot-product',
}

/**
 * Supported metadata return levels for a Vectorize query.
 */
export enum MetadataRetrievalLevel {
  /**
   * Full metadata for the vector return set, including all fields (including those un-indexed) without truncation.
   *
   * This is a more expensive retrieval, as it requires additional fetching & reading of un-indexed data.
   */
  ALL = 'all',
  /**
   * Return all metadata fields configured for indexing in the vector return set.
   *
   * This level of retrieval is "free" in that no additional overhead is incurred returning this data.
   * However, note that indexed metadata is subject to truncation (especially for larger strings).
   */
  INDEXED = 'indexed',
  /** No indexed metadata will be returned. */
  NONE = 'none',
}
