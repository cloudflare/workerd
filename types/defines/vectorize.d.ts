// Copyright (c) 2022-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

/**
 * Data types supported for holding vector metadata.
 */
type VectorizeVectorMetadataValue = string | number | boolean | string[];
/**
 * Additional information to associate with a vector.
 */
type VectorizeVectorMetadata =
  | VectorizeVectorMetadataValue
  | Record<string, VectorizeVectorMetadataValue>;

type VectorFloatArray = Float32Array | Float64Array;

interface VectorizeError {
  code?: number;
  error: string;
}

/**
 * Comparison logic/operation to use for metadata filtering.
 *
 * This list is expected to grow as support for more operations are released.
 */
type VectorizeVectorMetadataFilterOp = "$eq" | "$ne";

/**
 * Filter criteria for vector metadata used to limit the retrieved query result set.
 */
type VectorizeVectorMetadataFilter = {
  [field: string]:
    | Exclude<VectorizeVectorMetadataValue, string[]>
    | null
    | {
        [Op in VectorizeVectorMetadataFilterOp]?: Exclude<
          VectorizeVectorMetadataValue,
          string[]
        > | null;
      };
};

/**
 * Supported distance metrics for an index.
 * Distance metrics determine how other "similar" vectors are determined.
 */
type VectorizeDistanceMetric = "euclidean" | "cosine" | "dot-product";

/**
 * Metadata return levels for a Vectorize query.
 *
 * Default to "none".
 *
 * @property all      Full metadata for the vector return set, including all fields (including those un-indexed) without truncation. This is a more expensive retrieval, as it requires additional fetching & reading of un-indexed data.
 * @property indexed  Return all metadata fields configured for indexing in the vector return set. This level of retrieval is "free" in that no additional overhead is incurred returning this data. However, note that indexed metadata is subject to truncation (especially for larger strings).
 * @property none     No indexed metadata will be returned.
 */
type VectorizeMetadataRetrievalLevel = "all" | "indexed" | "none";

interface VectorizeQueryOptions<
  MetadataReturn extends boolean | VectorizeMetadataRetrievalLevel = boolean,
> {
  topK?: number;
  namespace?: string;
  returnValues?: boolean;
  returnMetadata?: MetadataReturn;
  filter?: VectorizeVectorMetadataFilter;
}

/**
 * Information about the configuration of an index.
 */
type VectorizeIndexConfig =
  | {
      dimensions: number;
      metric: VectorizeDistanceMetric;
    }
  | {
      preset: string; // keep this generic, as we'll be adding more presets in the future and this is only in a read capacity
    };

/**
 * Metadata about an existing index.
 *
 * This type is exclusively for the Vectorize **beta** and will be deprecated once Vectorize RC is released.
 * See {@link VectorizeIndexInfo} for its post-beta equivalent.
 */
interface VectorizeIndexDetails {
  /** The unique ID of the index */
  readonly id: string;
  /** The name of the index. */
  name: string;
  /** (optional) A human readable description for the index. */
  description?: string;
  /** The index configuration, including the dimension size and distance metric. */
  config: VectorizeIndexConfig;
  /** The number of records containing vectors within the index. */
  vectorsCount: number;
}

/**
 * Metadata about an existing index.
 */
interface VectorizeIndexInfo {
  /** The number of records containing vectors within the index. */
  vectorsCount: number;
  /** Number of dimensions the index has been configured for. */
  dimensions: number;
  /** ISO 8601 datetime of the last processed mutation on in the index. All changes before this mutation will be reflected in the index state. */
  processedUpToDatetime: number;
  /** UUIDv4 of the last mutation processed by the index. All changes before this mutation will be reflected in the index state. */
  processedUpToMutation: number;
}

/**
 * Represents a single vector value set along with its associated metadata.
 */
interface VectorizeVector {
  /** The ID for the vector. This can be user-defined, and must be unique. It should uniquely identify the object, and is best set based on the ID of what the vector represents. */
  id: string;
  /** The vector values */
  values: VectorFloatArray | number[];
  /** The namespace this vector belongs to. */
  namespace?: string;
  /** Metadata associated with the vector. Includes the values of other fields and potentially additional details. */
  metadata?: Record<string, VectorizeVectorMetadata>;
}

/**
 * Represents a matched vector for a query along with its score and (if specified) the matching vector information.
 */
type VectorizeMatch = Pick<Partial<VectorizeVector>, "values"> &
  Omit<VectorizeVector, "values"> & {
    /** The score or rank for similarity, when returned as a result */
    score: number;
  };

/**
 * A set of matching {@link VectorizeMatch} for a particular query.
 */
interface VectorizeMatches {
  matches: VectorizeMatch[];
  count: number;
}

/**
 * Results of an operation that performed a mutation on a set of vectors.
 * Here, `ids` is a list of vectors that were successfully processed.
 *
 * This type is exclusively for the Vectorize **beta** and will be deprecated once Vectorize RC is released.
 * See {@link VectorizeAsyncMutation} for its post-beta equivalent.
 */
interface VectorizeVectorMutation {
  /* List of ids of vectors that were successfully processed. */
  ids: string[];
  /* Total count of the number of processed vectors. */
  count: number;
}

/**
 * Result type indicating a mutation on the Vectorize Index.
 * Actual mutations are processed async where the `mutationId` is the unique identifier for the operation.
 */
interface VectorizeAsyncMutation {
  /** The unique identifier for the async mutation operation containing the changeset. */
  mutationId: string;
}

/**
 * A Vectorize Vector Search Index for querying vectors/embeddings.
 *
 * This type is exclusively for the Vectorize **beta** and will be deprecated once Vectorize RC is released.
 * See {@link Vectorize} for its new implementation.
 */
declare abstract class VectorizeIndex {
  /**
   * Get information about the currently bound index.
   * @returns A promise that resolves with information about the current index.
   */
  public describe(): Promise<VectorizeIndexDetails>;
  /**
   * Use the provided vector to perform a similarity search across the index.
   * @param vector Input vector that will be used to drive the similarity search.
   * @param options Configuration options to massage the returned data.
   * @returns A promise that resolves with matched and scored vectors.
   */
  public query(
    vector: VectorFloatArray | number[],
    options: VectorizeQueryOptions
  ): Promise<VectorizeMatches>;
  /**
   * Insert a list of vectors into the index dataset. If a provided id exists, an error will be thrown.
   * @param vectors List of vectors that will be inserted.
   * @returns A promise that resolves with the ids & count of records that were successfully processed.
   */
  public insert(vectors: VectorizeVector[]): Promise<VectorizeVectorMutation>;
  /**
   * Upsert a list of vectors into the index dataset. If a provided id exists, it will be replaced with the new values.
   * @param vectors List of vectors that will be upserted.
   * @returns A promise that resolves with the ids & count of records that were successfully processed.
   */
  public upsert(vectors: VectorizeVector[]): Promise<VectorizeVectorMutation>;
  /**
   * Delete a list of vectors with a matching id.
   * @param ids List of vector ids that should be deleted.
   * @returns A promise that resolves with the ids & count of records that were successfully processed (and thus deleted).
   */
  public deleteByIds(ids: string[]): Promise<VectorizeVectorMutation>;
  /**
   * Get a list of vectors with a matching id.
   * @param ids List of vector ids that should be returned.
   * @returns A promise that resolves with the raw unscored vectors matching the id set.
   */
  public getByIds(ids: string[]): Promise<VectorizeVector[]>;
}

/**
 * A Vectorize Vector Search Index for querying vectors/embeddings.
 *
 * Mutations in this version are async, returning a mutation id.
 */
declare abstract class Vectorize {
  /**
   * Get information about the currently bound index.
   * @returns A promise that resolves with information about the current index.
   */
  public describe(): Promise<VectorizeIndexInfo>;
  /**
   * Use the provided vector to perform a similarity search across the index.
   * @param vector Input vector that will be used to drive the similarity search.
   * @param options Configuration options to massage the returned data.
   * @returns A promise that resolves with matched and scored vectors.
   */
  public query(
    vector: VectorFloatArray | number[],
    options: VectorizeQueryOptions<VectorizeMetadataRetrievalLevel>
  ): Promise<VectorizeMatches>;
  /**
   * Insert a list of vectors into the index dataset. If a provided id exists, an error will be thrown.
   * @param vectors List of vectors that will be inserted.
   * @returns A promise that resolves with a unique identifier of a mutation containing the insert changeset.
   */
  public insert(vectors: VectorizeVector[]): Promise<VectorizeAsyncMutation>;
  /**
   * Upsert a list of vectors into the index dataset. If a provided id exists, it will be replaced with the new values.
   * @param vectors List of vectors that will be upserted.
   * @returns A promise that resolves with a unique identifier of a mutation containing the upsert changeset.
   */
  public upsert(vectors: VectorizeVector[]): Promise<VectorizeAsyncMutation>;
  /**
   * Delete a list of vectors with a matching id.
   * @param ids List of vector ids that should be deleted.
   * @returns A promise that resolves with a unique identifier of a mutation containing the delete changeset.
   */
  public deleteByIds(ids: string[]): Promise<VectorizeAsyncMutation>;
  /**
   * Get a list of vectors with a matching id.
   * @param ids List of vector ids that should be returned.
   * @returns A promise that resolves with the raw unscored vectors matching the id set.
   */
  public getByIds(ids: string[]): Promise<VectorizeVector[]>;
}
