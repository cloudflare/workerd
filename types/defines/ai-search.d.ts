// ============ AI Search Error Interfaces ============

export interface AiSearchInternalError extends Error {}
export interface AiSearchNotFoundError extends Error {}

// ============ AI Search Common Types ============

/** A single message in a conversation-style search or chat request. */
export type AiSearchMessage = {
  role: 'system' | 'developer' | 'user' | 'assistant' | 'tool';
  content: string | null;
};

/**
 * Common shape for `ai_search_options` used by both single-instance and multi-instance requests.
 * Contains retrieval, query rewrite, reranking, and cache sub-options.
 */
export type AiSearchOptions = {
  retrieval?: {
    /** Which retrieval backend to use. Defaults to the instance's configured index_method. */
    retrieval_type?: 'vector' | 'keyword' | 'hybrid';
    /** Fusion method for combining vector + keyword results. */
    fusion_method?: 'max' | 'rrf';
    /** How keyword terms are combined: "and" = all terms must match, "or" = any term matches. */
    keyword_match_mode?: 'and' | 'or';
    /** Minimum similarity score (0-1) for a result to be included. Default 0.4. */
    match_threshold?: number;
    /** Maximum number of results to return (1-50). Default 10. */
    max_num_results?: number;
    /** Vectorize metadata filters applied to the search. */
    filters?: VectorizeVectorMetadataFilter;
    /** Number of surrounding chunks to include for context (0-3). Default 0. */
    context_expansion?: number;
    /** If true, return only item metadata without chunk text. */
    metadata_only?: boolean;
    /** If true (default), return empty results on retrieval failure instead of throwing. */
    return_on_failure?: boolean;
    /** Boost results by metadata field values. Max 3 entries. */
    boost_by?: Array<{
      field: string;
      direction?: 'asc' | 'desc' | 'exists' | 'not_exists';
    }>;
    [key: string]: unknown;
  };
  query_rewrite?: {
    enabled?: boolean;
    model?: string;
    rewrite_prompt?: string;
    [key: string]: unknown;
  };
  reranking?: {
    enabled?: boolean;
    model?: string;
    /** Match threshold (0-1, default 0.4) */
    match_threshold?: number;
    [key: string]: unknown;
  };
  cache?: {
    enabled?: boolean;
    cache_threshold?:
      | 'super_strict_match'
      | 'close_enough'
      | 'flexible_friend'
      | 'anything_goes';
  };
  [key: string]: unknown;
};

// ============ AI Search Request Types ============

/**
 * Request body for single-instance search.
 * Exactly one of `query` or `messages` must be provided.
 */
export type AiSearchSearchRequest =
  | {
      /** Simple query string. */
      query: string;
      messages?: never;
      ai_search_options?: AiSearchOptions;
    }
  | {
      query?: never;
      /** Conversation-style input. At least one user message with non-empty content is required. */
      messages: AiSearchMessage[];
      ai_search_options?: AiSearchOptions;
    };

export type AiSearchChatCompletionsRequest = {
  messages: AiSearchMessage[];
  model?: string;
  stream?: boolean;
  ai_search_options?: AiSearchOptions;
  [key: string]: unknown;
};

// ============ AI Search Multi-Instance Types (Namespace-Scoped) ============

/** `ai_search_options` shape for multi-instance requests — requires `instance_ids`. */
export type AiSearchMultiSearchOptions = AiSearchOptions & {
  /** Instance IDs to search across (1-10). */
  instance_ids: string[];
};

/**
 * Request for searching across multiple instances within a namespace.
 * `ai_search_options` is required and must include `instance_ids`.
 * Exactly one of `query` or `messages` must be provided.
 */
export type AiSearchMultiSearchRequest =
  | {
      /** Simple query string. */
      query: string;
      messages?: never;
      ai_search_options: AiSearchMultiSearchOptions;
    }
  | {
      query?: never;
      /** Conversation-style input. */
      messages: AiSearchMessage[];
      ai_search_options: AiSearchMultiSearchOptions;
    };

/** A search result chunk tagged with the instance it originated from. */
export type AiSearchMultiSearchChunk =
  AiSearchSearchResponse['chunks'][number] & {
    instance_id: string;
  };

/** Describes a per-instance error during a multi-instance operation. */
export type AiSearchMultiSearchError = {
  instance_id: string;
  message: string;
};

/** Response from a multi-instance search, with chunks tagged by instance and optional partial-failure errors. */
export type AiSearchMultiSearchResponse = {
  search_query: string;
  chunks: AiSearchMultiSearchChunk[];
  errors?: AiSearchMultiSearchError[];
};

/** Request for chat completions across multiple instances within a namespace. `ai_search_options` is required and must include `instance_ids`. */
export type AiSearchMultiChatCompletionsRequest = Omit<
  AiSearchChatCompletionsRequest,
  'ai_search_options'
> & {
  ai_search_options: AiSearchMultiSearchOptions;
};

/** Response from multi-instance chat completions, with chunks tagged by instance and optional partial-failure errors. */
export type AiSearchMultiChatCompletionsResponse = Omit<
  AiSearchChatCompletionsResponse,
  'chunks'
> & {
  chunks: AiSearchMultiSearchChunk[];
  errors?: AiSearchMultiSearchError[];
};

// ============ AI Search Response Types ============

export type AiSearchSearchResponse = {
  search_query: string;
  chunks: Array<{
    id: string;
    type: string;
    /** Match score (0-1) */
    score: number;
    text: string;
    item: {
      timestamp?: number;
      key: string;
      metadata?: Record<string, unknown>;
    };
    scoring_details?: {
      /** Keyword match score (0-1) */
      keyword_score?: number;
      /** Vector similarity score (0-1) */
      vector_score?: number;
      /** Keyword rank position */
      keyword_rank?: number;
      /** Vector rank position */
      vector_rank?: number;
      /** Reranking model score */
      reranking_score?: number;
      /** Fusion method used to combine results */
      fusion_method?: 'rrf' | 'max';
      [key: string]: unknown;
    };
  }>;
};

export type AiSearchChatCompletionsResponse = {
  id?: string;
  object?: string;
  model?: string;
  choices: Array<{
    index?: number;
    message: {
      role: 'system' | 'developer' | 'user' | 'assistant' | 'tool';
      content: string | null;
      [key: string]: unknown;
    };
    [key: string]: unknown;
  }>;
  chunks: AiSearchSearchResponse['chunks'];
  [key: string]: unknown;
};

export type AiSearchStatsResponse = {
  queued?: number;
  running?: number;
  completed?: number;
  error?: number;
  skipped?: number;
  outdated?: number;
  last_activity?: string;
  /** Storage engine statistics. */
  engine?: {
    vectorize?: {
      vectorsCount: number;
      dimensions: number;
    };
    r2?: {
      payloadSizeBytes: number;
      metadataSizeBytes: number;
      objectCount: number;
    };
  };
};

// ============ AI Search Instance Info Types ============

export type AiSearchInstanceInfo = {
  id: string;
  type?: 'r2' | 'web-crawler' | string;
  source?: string;
  source_params?: unknown;
  paused?: boolean;
  status?: string;
  namespace?: string;
  created_at?: string;
  modified_at?: string;
  token_id?: string;
  ai_gateway_id?: string;
  rewrite_query?: boolean;
  reranking?: boolean;
  embedding_model?: string;
  ai_search_model?: string;
  rewrite_model?: string;
  reranking_model?: string;
  /** @deprecated Use index_method instead. */
  hybrid_search_enabled?: boolean;
  /** Controls which storage backends are active. */
  index_method?: { vector?: boolean; keyword?: boolean };
  /** Fusion method for combining vector and keyword results. */
  fusion_method?: 'max' | 'rrf';
  indexing_options?: { keyword_tokenizer?: 'porter' | 'trigram' } | null;
  retrieval_options?: {
    keyword_match_mode?: 'and' | 'or';
    boost_by?: Array<{
      field: string;
      direction?: 'asc' | 'desc' | 'exists' | 'not_exists';
    }>;
  } | null;
  chunk?: boolean;
  chunk_size?: number;
  chunk_overlap?: number;
  score_threshold?: number;
  max_num_results?: number;
  cache?: boolean;
  cache_threshold?:
    | 'super_strict_match'
    | 'close_enough'
    | 'flexible_friend'
    | 'anything_goes';
  custom_metadata?: Array<{
    field_name: string;
    data_type: 'text' | 'number' | 'boolean' | 'datetime';
  }>;
  /** Sync interval in seconds. */
  sync_interval?: 3600 | 7200 | 14400 | 21600 | 43200 | 86400;
  metadata?: Record<string, unknown>;
  [key: string]: unknown;
};

/** Pagination, search, and ordering parameters for listing instances within a namespace. */
export type AiSearchListInstancesParams = {
  page?: number;
  per_page?: number;
  /** Search instances by ID. */
  search?: string;
  /** Field to sort by. */
  order_by?: 'created_at';
  /** Sort direction. */
  order_by_direction?: 'asc' | 'desc';
};

export type AiSearchListResponse = {
  result: AiSearchInstanceInfo[];
  result_info?: {
    count: number;
    page: number;
    per_page: number;
    total_count: number;
  };
};

// ============ AI Search Config Types ============

export type AiSearchConfig = {
  /** Instance ID (1-32 chars, pattern: ^[a-z0-9_]+(?:-[a-z0-9_]+)*$) */
  id: string;
  /** Instance type. Omit to create with built-in storage. */
  type?: 'r2' | 'web-crawler' | string;
  /** Source URL (required for web-crawler type). */
  source?: string;
  source_params?: unknown;
  /** Token ID (UUID format) */
  token_id?: string;
  ai_gateway_id?: string;
  /** Enable query rewriting (default false) */
  rewrite_query?: boolean;
  /** Enable reranking (default false) */
  reranking?: boolean;
  embedding_model?: string;
  ai_search_model?: string;
  rewrite_model?: string;
  reranking_model?: string;
  /** @deprecated Use index_method instead. */
  hybrid_search_enabled?: boolean;
  /** Controls which storage backends are used during indexing. Defaults to vector-only. */
  index_method?: { vector?: boolean; keyword?: boolean };
  /** Fusion method for combining vector and keyword results. "rrf" = reciprocal rank fusion (default), "max" = maximum score. */
  fusion_method?: 'max' | 'rrf';
  indexing_options?: { keyword_tokenizer?: 'porter' | 'trigram' } | null;
  retrieval_options?: {
    keyword_match_mode?: 'and' | 'or';
    boost_by?: Array<{
      field: string;
      direction?: 'asc' | 'desc' | 'exists' | 'not_exists';
    }>;
  } | null;
  chunk?: boolean;
  chunk_size?: number;
  chunk_overlap?: number;
  /** Minimum similarity score (0-1) for a result to be included. */
  score_threshold?: number;
  max_num_results?: number;
  cache?: boolean;
  /** Similarity threshold for cache hits. Stricter = fewer cache hits but higher relevance. */
  cache_threshold?:
    | 'super_strict_match'
    | 'close_enough'
    | 'flexible_friend'
    | 'anything_goes';
  custom_metadata?: Array<{
    field_name: string;
    data_type: 'text' | 'number' | 'boolean' | 'datetime';
  }>;
  namespace?: string;
  /** Sync interval in seconds. 3600=1h, 7200=2h, 14400=4h, 21600=6h, 43200=12h, 86400=24h. */
  sync_interval?: 3600 | 7200 | 14400 | 21600 | 43200 | 86400;
  metadata?: Record<string, unknown>;
  [key: string]: unknown;
};

// ============ AI Search Item Types ============

export type AiSearchItemInfo = {
  id: string;
  key: string;
  status: 'completed' | 'error' | 'skipped' | 'queued' | 'running' | 'outdated';
  next_action?: 'INDEX' | 'DELETE' | null;
  error?: string;
  checksum?: string;
  namespace?: string;
  chunks_count?: number | null;
  file_size?: number | null;
  source_id?: string | null;
  last_seen_at?: string;
  created_at?: string;
  metadata?: Record<string, unknown>;
  [key: string]: unknown;
};

export type AiSearchItemContentResult = {
  body: ReadableStream;
  contentType: string;
  filename: string;
  size: number;
};

export type AiSearchUploadItemOptions = {
  metadata?: Record<string, unknown>;
};

export type AiSearchListItemsParams = {
  page?: number;
  per_page?: number;
  /** Search items by key name. */
  search?: string;
  /** Sort order for results. */
  sort_by?: 'status' | 'modified_at';
  /** Filter items by processing status. */
  status?:
    | 'queued'
    | 'running'
    | 'completed'
    | 'error'
    | 'skipped'
    | 'outdated';
  /** Filter items by source (e.g. "builtin" or "web-crawler:https://example.com"). */
  source?: string;
  /** JSON-encoded Vectorize filter for metadata filtering. */
  metadata_filter?: string;
};

export type AiSearchListItemsResponse = {
  result: AiSearchItemInfo[];
  result_info?: {
    count: number;
    page: number;
    per_page: number;
    total_count: number;
  };
};

// ============ AI Search Item Logs Types ============

export type AiSearchItemLogsParams = {
  /** Maximum number of log entries to return (1-100, default 50). */
  limit?: number;
  /** Opaque cursor for pagination. Pass the `cursor` value from a previous response. */
  cursor?: string;
};

export type AiSearchItemLog = {
  timestamp: string;
  action: string;
  message: string;
  fileKey?: string;
  chunkCount?: number;
  processingTimeMs?: number;
  errorType?: string;
};

/** Paginated response for item processing logs (cursor-based). */
export type AiSearchItemLogsResponse = {
  result: AiSearchItemLog[];
  result_info: {
    count: number;
    per_page: number;
    cursor: string | null;
    truncated: boolean;
  };
};

// ============ AI Search Item Chunks Types ============

export type AiSearchItemChunksParams = {
  /** Maximum number of chunks to return (1-100, default 20). */
  limit?: number;
  /** Offset into the chunks list (default 0). */
  offset?: number;
};

/** A single indexed chunk belonging to an item, including its text content and byte range. */
export type AiSearchItemChunk = {
  id: string;
  text: string;
  start_byte: number;
  end_byte: number;
  item?: {
    timestamp?: number;
    key: string;
    metadata?: Record<string, unknown>;
  };
};

/** Paginated response for item chunks (offset-based). */
export type AiSearchItemChunksResponse = {
  result: AiSearchItemChunk[];
  result_info: {
    count: number;
    total: number;
    limit: number;
    offset: number;
  };
};

// ============ AI Search Job Types ============

export type AiSearchJobInfo = {
  id: string;
  source: 'user' | 'schedule';
  description?: string;
  last_seen_at?: string;
  started_at?: string;
  ended_at?: string;
  end_reason?: string;
};

export type AiSearchJobLog = {
  id: number;
  message: string;
  message_type: number;
  created_at: number;
};

export type AiSearchCreateJobParams = {
  description?: string;
};

export type AiSearchListJobsParams = {
  page?: number;
  per_page?: number;
};

export type AiSearchListJobsResponse = {
  result: AiSearchJobInfo[];
  result_info?: {
    count: number;
    page: number;
    per_page: number;
    total_count: number;
  };
};

export type AiSearchJobLogsParams = {
  page?: number;
  per_page?: number;
};

export type AiSearchJobLogsResponse = {
  result: AiSearchJobLog[];
  result_info?: {
    count: number;
    page: number;
    per_page: number;
    total_count: number;
  };
};

// ============ AI Search Sub-Service Classes ============

/**
 * Single item service for an AI Search instance.
 * Provides info, download, sync, logs, and chunks operations on a specific item.
 */
export declare abstract class AiSearchItem {
  /** Get metadata about this item. */
  info(): Promise<AiSearchItemInfo>;

  /**
   * Download the item's content.
   * @returns Object with body stream, content type, filename, and size.
   */
  download(): Promise<AiSearchItemContentResult>;

  /**
   * Trigger re-indexing of this item.
   * @returns The updated item info.
   */
  sync(): Promise<AiSearchItemInfo>;

  /**
   * Retrieve processing logs for this item (cursor-based pagination).
   * @param params Optional pagination parameters (limit, cursor).
   * @returns Paginated log entries for this item.
   */
  logs(params?: AiSearchItemLogsParams): Promise<AiSearchItemLogsResponse>;

  /**
   * List indexed chunks for this item (offset-based pagination).
   * @param params Optional pagination parameters (limit, offset).
   * @returns Paginated chunk entries for this item.
   */
  chunks(
    params?: AiSearchItemChunksParams
  ): Promise<AiSearchItemChunksResponse>;
}

/**
 * Items collection service for an AI Search instance.
 * Provides list, upload, and access to individual items.
 */
export declare abstract class AiSearchItems {
  /** List items in this instance. */
  list(params?: AiSearchListItemsParams): Promise<AiSearchListItemsResponse>;

  /**
   * Upload a file as an item. Behaves as an upsert: if an item with the same
   * filename already exists, it is overwritten and re-indexed.
   * @param name Filename for the uploaded item.
   * @param content File content as a ReadableStream, Blob, or string.
   * @param options Optional metadata to attach to the item.
   * @returns The created item info.
   */
  upload(
    name: string,
    content: ReadableStream | Blob | string,
    options?: AiSearchUploadItemOptions
  ): Promise<AiSearchItemInfo>;

  /**
   * Upload a file and poll until processing completes.
   * Behaves as an upsert: if an item with the same filename already exists,
   * it is overwritten and re-indexed.
   * @param name Filename for the uploaded item.
   * @param content File content as a ReadableStream, Blob, or string.
   * @param options Optional metadata and polling configuration.
   * @returns The item info after processing completes (or timeout).
   */
  uploadAndPoll(
    name: string,
    content: ReadableStream | Blob | string,
    options?: AiSearchUploadItemOptions & {
      /** Polling interval in milliseconds (default 1000). */
      pollIntervalMs?: number;
      /** Maximum time to wait in milliseconds (default 30000). */
      timeoutMs?: number;
    }
  ): Promise<AiSearchItemInfo>;

  /**
   * Get an item by ID.
   * @param itemId The item identifier.
   * @returns Item service for info, download, sync, logs, and chunks operations.
   */
  get(itemId: string): AiSearchItem;

  /**
   * Delete an item from the instance.
   * @param itemId The item identifier.
   */
  delete(itemId: string): Promise<void>;
}

/**
 * Single job service for an AI Search instance.
 * Provides info, logs, and cancel operations for a specific job.
 */
export declare abstract class AiSearchJob {
  /** Get metadata about this job. */
  info(): Promise<AiSearchJobInfo>;

  /** Get logs for this job. */
  logs(params?: AiSearchJobLogsParams): Promise<AiSearchJobLogsResponse>;

  /**
   * Cancel a running job.
   * @returns The updated job info.
   * @throws AiSearchNotFoundError if the job does not exist.
   */
  cancel(): Promise<AiSearchJobInfo>;
}

/**
 * Jobs collection service for an AI Search instance.
 * Provides list, create, and access to individual jobs.
 */
export declare abstract class AiSearchJobs {
  /** List jobs for this instance. */
  list(params?: AiSearchListJobsParams): Promise<AiSearchListJobsResponse>;

  /**
   * Create a new indexing job.
   * @param params Optional job parameters.
   * @returns The created job info.
   */
  create(params?: AiSearchCreateJobParams): Promise<AiSearchJobInfo>;

  /**
   * Get a job by ID.
   * @param jobId The job identifier.
   * @returns Job service for info, logs, and cancel operations.
   */
  get(jobId: string): AiSearchJob;
}

// ============ AI Search Binding Classes ============

/**
 * Instance-level AI Search service.
 *
 * Used as:
 * - The return type of `AiSearchNamespace.get(name)` (namespace binding)
 * - The type of `env.BLOG_SEARCH` (single instance binding via `ai_search`)
 *
 * Provides search, chat, update, stats, items, and jobs operations.
 *
 * @example
 * ```ts
 * // Via namespace binding
 * const instance = env.AI_SEARCH.get("blog");
 * const results = await instance.search({
 *   query: "How does caching work?",
 * });
 *
 * // Via single instance binding
 * const results = await env.BLOG_SEARCH.search({
 *   messages: [{ role: "user", content: "How does caching work?" }],
 * });
 * ```
 */
export declare abstract class AiSearchInstance {
  /**
   * Search the AI Search instance for relevant chunks.
   * @param params Search request with query or messages and optional AI search options.
   * @returns Search response with matching chunks and search query.
   */
  search(params: AiSearchSearchRequest): Promise<AiSearchSearchResponse>;

  /**
   * Generate chat completions with AI Search context (streaming).
   * @param params Chat completions request with stream: true.
   * @returns ReadableStream of server-sent events.
   */
  chatCompletions(
    params: AiSearchChatCompletionsRequest & { stream: true }
  ): Promise<ReadableStream>;

  /**
   * Generate chat completions with AI Search context.
   * @param params Chat completions request.
   * @returns Chat completion response with choices and RAG chunks.
   */
  chatCompletions(
    params: AiSearchChatCompletionsRequest
  ): Promise<AiSearchChatCompletionsResponse>;

  /**
   * Update the instance configuration.
   * @param config Partial configuration to update.
   * @returns Updated instance info.
   */
  update(config: Partial<AiSearchConfig>): Promise<AiSearchInstanceInfo>;

  /** Get metadata about this instance. */
  info(): Promise<AiSearchInstanceInfo>;

  /**
   * Get instance statistics (item count, indexing status, etc.).
   * @returns Statistics with counts per status, last activity time, and engine details.
   */
  stats(): Promise<AiSearchStatsResponse>;

  /** Items collection — list, upload, and manage items in this instance. */
  get items(): AiSearchItems;

  /** Jobs collection — list, create, and inspect indexing jobs. */
  get jobs(): AiSearchJobs;
}

/**
 * Namespace-level AI Search service.
 *
 * Used as the type of `env.AI_SEARCH` (namespace binding via `ai_search_namespaces`).
 * Scoped to a single namespace. Provides dynamic instance access, creation, deletion,
 * and multi-instance search/chat operations.
 *
 * @example
 * ```ts
 * // Access an instance within the namespace
 * const blog = env.AI_SEARCH.get("blog");
 * const results = await blog.search({ query: "How does caching work?" });
 *
 * // List all instances in the namespace
 * const instances = await env.AI_SEARCH.list();
 *
 * // Create a new instance with built-in storage
 * const tenant = await env.AI_SEARCH.create({ id: "tenant-123" });
 *
 * // Upload items into the instance
 * await tenant.items.upload("doc.pdf", fileContent);
 *
 * // Search across multiple instances
 * const multi = await env.AI_SEARCH.search({
 *   query: "caching",
 *   ai_search_options: { instance_ids: ["blog", "docs"] },
 * });
 *
 * // Delete an instance
 * await env.AI_SEARCH.delete("tenant-123");
 * ```
 */
export declare abstract class AiSearchNamespace {
  /**
   * Get an instance by name within the bound namespace.
   * @param name Instance name.
   * @returns Instance service for search, chat, update, stats, items, and jobs.
   */
  get(name: string): AiSearchInstance;

  /**
   * List instances in the bound namespace.
   * @param params Optional pagination, search, and ordering parameters.
   * @returns Array of instance metadata with pagination info.
   */
  list(params?: AiSearchListInstancesParams): Promise<AiSearchListResponse>;

  /**
   * Create a new instance within the bound namespace.
   * @param config Instance configuration. Only `id` is required — omit `type` and `source` to create with built-in storage.
   * @returns Instance service for the newly created instance.
   *
   * @example
   * ```ts
   * // Create with built-in storage (upload items manually)
   * const instance = await env.AI_SEARCH.create({ id: "my-search" });
   *
   * // Create with web crawler source
   * const instance = await env.AI_SEARCH.create({
   *   id: "docs-search",
   *   type: "web-crawler",
   *   source: "https://developers.cloudflare.com",
   * });
   * ```
   */
  create(config: AiSearchConfig): Promise<AiSearchInstance>;

  /**
   * Delete an instance from the bound namespace.
   * @param name Instance name to delete.
   */
  delete(name: string): Promise<void>;

  /**
   * Search across multiple instances within the bound namespace.
   * Fans out to the specified instance_ids and merges results.
   * @param params Search request with required `ai_search_options.instance_ids`.
   * @returns Search response with chunks tagged by instance_id and optional partial-failure errors.
   */
  search(
    params: AiSearchMultiSearchRequest
  ): Promise<AiSearchMultiSearchResponse>;

  /**
   * Generate chat completions across multiple instances within the bound namespace (streaming).
   * Fans out to the specified instance_ids, merges context, and generates a response.
   * @param params Chat completions request with stream: true and required `ai_search_options.instance_ids`.
   * @returns ReadableStream of server-sent events.
   */
  chatCompletions(
    params: AiSearchMultiChatCompletionsRequest & { stream: true }
  ): Promise<ReadableStream>;

  /**
   * Generate chat completions across multiple instances within the bound namespace.
   * Fans out to the specified instance_ids, merges context, and generates a response.
   * @param params Chat completions request with required `ai_search_options.instance_ids`.
   * @returns Chat completion response with choices, chunks tagged by instance_id, and optional partial-failure errors.
   */
  chatCompletions(
    params: AiSearchMultiChatCompletionsRequest
  ): Promise<AiSearchMultiChatCompletionsResponse>;
}
