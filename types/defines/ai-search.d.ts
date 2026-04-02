// ============ AI Search Error Interfaces ============

export interface AiSearchInternalError extends Error {}
export interface AiSearchNotFoundError extends Error {}

// ============ AI Search Request Types ============

export type AiSearchSearchRequest = {
  messages: Array<{
    role: 'system' | 'developer' | 'user' | 'assistant' | 'tool';
    content: string | null;
  }>;
  ai_search_options?: {
    retrieval?: {
      retrieval_type?: 'vector' | 'keyword' | 'hybrid';
      /** Match threshold (0-1, default 0.4) */
      match_threshold?: number;
      /** Maximum number of results (1-50, default 10) */
      max_num_results?: number;
      filters?: VectorizeVectorMetadataFilter;
      /** Context expansion (0-3, default 0) */
      context_expansion?: number;
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
      model?: '@cf/baai/bge-reranker-base' | string;
      /** Match threshold (0-1, default 0.4) */
      match_threshold?: number;
      [key: string]: unknown;
    };
    [key: string]: unknown;
  };
};

export type AiSearchChatCompletionsRequest = {
  messages: Array<{
    role: 'system' | 'developer' | 'user' | 'assistant' | 'tool';
    content: string | null;
    [key: string]: unknown;
  }>;
  model?: string;
  stream?: boolean;
  ai_search_options?: {
    retrieval?: {
      retrieval_type?: 'vector' | 'keyword' | 'hybrid';
      match_threshold?: number;
      max_num_results?: number;
      filters?: VectorizeVectorMetadataFilter;
      context_expansion?: number;
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
      model?: '@cf/baai/bge-reranker-base' | string;
      match_threshold?: number;
      [key: string]: unknown;
    };
    [key: string]: unknown;
  };
  [key: string]: unknown;
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
};

// ============ AI Search Instance Info Types ============

export type AiSearchInstanceInfo = {
  id: string;
  type?: 'r2' | 'web-crawler' | string;
  source?: string;
  paused?: boolean;
  status?: string;
  namespace?: string;
  created_at?: string;
  modified_at?: string;
  [key: string]: unknown;
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
  [key: string]: unknown;
};

// ============ AI Search Item Types ============

export type AiSearchItemInfo = {
  id: string;
  key: string;
  status: 'completed' | 'error' | 'skipped' | 'queued' | 'processing' | 'outdated';
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
 * Provides info, delete, and download operations on a specific item.
 */
export declare abstract class AiSearchItem {
  /** Get metadata about this item. */
  info(): Promise<AiSearchItemInfo>;

  /**
   * Download the item's content.
   * @returns Object with body stream, content type, filename, and size.
   */
  download(): Promise<AiSearchItemContentResult>;
}

/**
 * Items collection service for an AI Search instance.
 * Provides list, upload, and access to individual items.
 */
export declare abstract class AiSearchItems {
  /** List items in this instance. */
  list(params?: AiSearchListItemsParams): Promise<AiSearchListItemsResponse>;

  /**
   * Upload a file as an item.
   * @param name Filename for the uploaded item.
   * @param content File content as a ReadableStream, ArrayBuffer, or string.
   * @param options Optional metadata to attach to the item.
   * @returns The created item info.
   */
  upload(
    name: string,
    content: ReadableStream | ArrayBuffer | string,
    options?: AiSearchUploadItemOptions
  ): Promise<AiSearchItemInfo>;

  /**
   * Upload a file and poll until processing completes.
   * @param name Filename for the uploaded item.
   * @param content File content as a ReadableStream, ArrayBuffer, or string.
   * @param options Optional metadata to attach to the item.
   * @returns The item info after processing completes (or timeout).
   */
  uploadAndPoll(
    name: string,
    content: ReadableStream | ArrayBuffer | string,
    options?: AiSearchUploadItemOptions
  ): Promise<AiSearchItemInfo>;

  /**
   * Get an item by ID.
   * @param itemId The item identifier.
   * @returns Item service for info, delete, and download operations.
   */
  get(itemId: string): AiSearchItem;

  /** Delete this item from the instance.
   * @param itemId The item identifier.
   */
  delete(itemId: string): Promise<void>;
}

/**
 * Single job service for an AI Search instance.
 * Provides info and logs for a specific job.
 */
export declare abstract class AiSearchJob {
  /** Get metadata about this job. */
  info(): Promise<AiSearchJobInfo>;

  /** Get logs for this job. */
  logs(params?: AiSearchJobLogsParams): Promise<AiSearchJobLogsResponse>;
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
   * @returns Job service for info and logs operations.
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
 *   messages: [{ role: "user", content: "How does caching work?" }],
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
   * @param params Search request with messages and optional AI search options.
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
   * @returns Statistics with counts per status and last activity time.
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
 * Scoped to a single namespace. Provides dynamic instance access, creation, and deletion.
 *
 * @example
 * ```ts
 * // Access an instance within the namespace
 * const blog = env.AI_SEARCH.get("blog");
 * const results = await blog.search({
 *   messages: [{ role: "user", content: "How does caching work?" }],
 * });
 *
 * // List all instances in the namespace
 * const instances = await env.AI_SEARCH.list();
 *
 * // Create a new instance with built-in storage
 * const tenant = await env.AI_SEARCH.create({
 *   id: "tenant-123",
 * });
 *
 * // Upload items into the instance
 * await tenant.items.upload("doc.pdf", fileContent);
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
   * List all instances in the bound namespace.
   * @returns Array of instance metadata.
   */
  list(): Promise<AiSearchListResponse>;

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
}
