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

interface VectorizeQueryOptions {
  topK?: number;
  namespace?: string;
  returnValues?: boolean;
  returnMetadata?: boolean;
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
 * Represents a single vector value set along with its associated metadata.
 */
interface VectorizeVector {
  /** The ID for the vector. This can be user-defined, and must be unique. It should uniquely identify the object, and is best set based on the ID of what the vector represents. */
  id: string;
  /** The vector values */
  values: VectorFloatArray | number[];
  /** The namespace this vector belongs to. */
  namespace?: string;
  /** Metadata associated with the vector. Includes the values of the other fields and potentially additional details. */
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
 * A set of vector {@link VectorizeMatch} for a particular query.
 */
interface VectorizeMatches {
  matches: VectorizeMatch[];
  count: number;
}

/**
 * Results of an operation that performed a mutation on a set of vectors.
 * Here, `ids` is a list of vectors that were successfully processed.
 */
interface VectorizeVectorMutation {
  /* List of ids of vectors that were successfully processed. */
  ids: string[];
  /* Total count of the number of processed vectors. */
  count: number;
}

/**
* Results of an operation that performed a mutation on a set of vectors
* with the v2 version of Vectorize.
* Here, `mutationId` is the identifier for the last mutation processed by Vectorize.
*/
interface VectorizeVectorMutationV2 {
  /* The identifier for the last mutation processed by Vectorize. */
  mutationId: string;
}

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
