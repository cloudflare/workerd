/**
 * @deprecated AutoRAG has been replaced by AI Search. Use AiSearchInternalError instead.
 * @see AiSearchInternalError
 */
export interface AutoRAGInternalError extends Error {}

/**
 * @deprecated AutoRAG has been replaced by AI Search. Use AiSearchNotFoundError instead.
 * @see AiSearchNotFoundError
 */
export interface AutoRAGNotFoundError extends Error {}

/**
 * @deprecated This error type is no longer used in the AI Search API.
 */
export interface AutoRAGUnauthorizedError extends Error {}

/**
 * @deprecated AutoRAG has been replaced by AI Search. Use AiSearchNameNotSetError instead.
 * @see AiSearchNameNotSetError
 */
export interface AutoRAGNameNotSetError extends Error {}

/**
 * @deprecated AutoRAG has been replaced by AI Search.
 * Use AiSearchSearchRequest with the new API instead.
 * @see AiSearchSearchRequest
 */
export type AutoRagSearchRequest = {
  query: string;
  filters?: CompoundFilter | ComparisonFilter;
  max_num_results?: number;
  ranking_options?: {
    ranker?: string;
    score_threshold?: number;
  };
  reranking?: {
    enabled?: boolean;
    model?: string;
  };
  rewrite_query?: boolean;
};

/**
 * @deprecated AutoRAG has been replaced by AI Search.
 * Use AiSearchChatCompletionsRequest with the new API instead.
 * @see AiSearchChatCompletionsRequest
 */
export type AutoRagAiSearchRequest = AutoRagSearchRequest & {
  stream?: boolean;
  system_prompt?: string;
};

/**
 * @deprecated AutoRAG has been replaced by AI Search.
 * Use AiSearchChatCompletionsRequest with stream: true instead.
 * @see AiSearchChatCompletionsRequest
 */
export type AutoRagAiSearchRequestStreaming = Omit<
  AutoRagAiSearchRequest,
  'stream'
> & {
  stream: true;
};

/**
 * @deprecated AutoRAG has been replaced by AI Search.
 * Use AiSearchSearchResponse with the new API instead.
 * @see AiSearchSearchResponse
 */
export type AutoRagSearchResponse = {
  object: 'vector_store.search_results.page';
  search_query: string;
  data: {
    file_id: string;
    filename: string;
    score: number;
    attributes: Record<string, string | number | boolean | null>;
    content: {
      type: 'text';
      text: string;
    }[];
  }[];
  has_more: boolean;
  next_page: string | null;
};

/**
 * @deprecated AutoRAG has been replaced by AI Search.
 * Use AiSearchListResponse with the new API instead.
 * @see AiSearchListResponse
 */
export type AutoRagListResponse = {
  id: string;
  enable: boolean;
  type: string;
  source: string;
  vectorize_name: string;
  paused: boolean;
  status: string;
}[];

/**
 * @deprecated AutoRAG has been replaced by AI Search.
 * The new API returns different response formats for chat completions.
 */
export type AutoRagAiSearchResponse = AutoRagSearchResponse & {
  response: string;
};

/**
 * @deprecated AutoRAG has been replaced by AI Search.
 * Use the new AI Search API instead: `env.AI.aiSearch`
 *
 * Migration guide:
 * - `env.AI.autorag().list()` → `env.AI.aiSearch.list()`
 * - `env.AI.autorag('id').search(...)` → `env.AI.aiSearch.get('id').search(...)`
 * - `env.AI.autorag('id').aiSearch(...)` → `env.AI.aiSearch.get('id').chatCompletions(...)`
 *
 * @see AiSearchAccountService
 * @see AiSearchInstanceService
 */
export declare abstract class AutoRAG {
  /**
   * @deprecated Use `env.AI.aiSearch.list()` instead.
   * @see AiSearchAccountService.list
   */
  list(): Promise<AutoRagListResponse>;

  /**
   * @deprecated Use `env.AI.aiSearch.get(id).search(...)` instead.
   * Note: The new API uses a messages array instead of a query string.
   * @see AiSearchInstanceService.search
   */
  search(params: AutoRagSearchRequest): Promise<AutoRagSearchResponse>;

  /**
   * @deprecated Use `env.AI.aiSearch.get(id).chatCompletions(...)` instead.
   * @see AiSearchInstanceService.chatCompletions
   */
  aiSearch(params: AutoRagAiSearchRequestStreaming): Promise<Response>;

  /**
   * @deprecated Use `env.AI.aiSearch.get(id).chatCompletions(...)` instead.
   * @see AiSearchInstanceService.chatCompletions
   */
  aiSearch(params: AutoRagAiSearchRequest): Promise<AutoRagAiSearchResponse>;

  /**
   * @deprecated Use `env.AI.aiSearch.get(id).chatCompletions(...)` instead.
   * @see AiSearchInstanceService.chatCompletions
   */
  aiSearch(
    params: AutoRagAiSearchRequest
  ): Promise<AutoRagAiSearchResponse | Response>;
}
