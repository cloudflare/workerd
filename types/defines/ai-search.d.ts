// AI Search V2 API Error Interfaces
export interface AiSearchInternalError extends Error {}
export interface AiSearchNotFoundError extends Error {}
export interface AiSearchNameNotSetError extends Error {}

// Filter types (shared with AutoRAG for compatibility)
export type ComparisonFilter = {
  key: string;
  type: 'eq' | 'ne' | 'gt' | 'gte' | 'lt' | 'lte';
  value: string | number | boolean;
};

export type CompoundFilter = {
  type: 'and' | 'or';
  filters: ComparisonFilter[];
};

// AI Search V2 Request Types
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
      filters?: CompoundFilter | ComparisonFilter;
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
      /** Enable reranking (default false) */
      enabled?: boolean;
      model?: '@cf/baai/bge-reranker-base' | '';
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
  }>;
  model?: string;
  stream?: boolean;
  ai_search_options?: {
    retrieval?: {
      retrieval_type?: 'vector' | 'keyword' | 'hybrid';
      match_threshold?: number;
      max_num_results?: number;
      filters?: CompoundFilter | ComparisonFilter;
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
      model?: '@cf/baai/bge-reranker-base' | '';
      match_threshold?: number;
      [key: string]: unknown;
    };
    [key: string]: unknown;
  };
  [key: string]: unknown;
};

// AI Search V2 Response Types
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
    };
  }>;
};

export type AiSearchListResponse = Array<{
  id: string;
  internal_id?: string;
  account_id?: string;
  account_tag?: string;
  /** Whether the instance is enabled (default true) */
  enable?: boolean;
  type?: 'r2' | 'web-crawler';
  source?: string;
  [key: string]: unknown;
}>;

export type AiSearchConfig = {
  /** Instance ID (1-32 chars, pattern: ^[a-z0-9_]+(?:-[a-z0-9_]+)*$) */
  id: string;
  type: 'r2' | 'web-crawler';
  source: string;
  source_params?: object;
  /** Token ID (UUID format) */
  token_id?: string;
  ai_gateway_id?: string;
  /** Enable query rewriting (default false) */
  rewrite_query?: boolean;
  /** Enable reranking (default false) */
  reranking?: boolean;
  embedding_model?: string;
  ai_search_model?: string;
};

export type AiSearchInstance = {
  id: string;
  enable?: boolean;
  type?: 'r2' | 'web-crawler';
  source?: string;
  [key: string]: unknown;
};

// AI Search Instance Service - Instance-level operations
export declare abstract class AiSearchInstanceService {
  /**
   * Search the AI Search instance for relevant chunks.
   * @param params Search request with messages and AI search options
   * @returns Search response with matching chunks
   */
  search(params: AiSearchSearchRequest): Promise<AiSearchSearchResponse>;

  /**
   * Generate chat completions with AI Search context.
   * @param params Chat completions request with optional streaming
   * @returns Response object (if streaming) or chat completion result
   */
  chatCompletions(
    params: AiSearchChatCompletionsRequest
  ): Promise<Response | object>;

  /**
   * Delete this AI Search instance.
   */
  delete(): Promise<void>;
}

// AI Search Account Service - Account-level operations
export declare abstract class AiSearchAccountService {
  /**
   * List all AI Search instances in the account.
   * @returns Array of AI Search instances
   */
  list(): Promise<AiSearchListResponse>;

  /**
   * Get an AI Search instance by ID.
   * @param name Instance ID
   * @returns Instance service for performing operations
   */
  get(name: string): AiSearchInstanceService;

  /**
   * Create a new AI Search instance.
   * @param config Instance configuration
   * @returns Instance service for performing operations
   */
  create(config: AiSearchConfig): Promise<AiSearchInstanceService>;
}
