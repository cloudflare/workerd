/**
 * @deprecated Use the standalone AI Search Workers binding instead.
 * See https://developers.cloudflare.com/ai-search/usage/workers-binding/
 */
export interface AutoRAGInternalError extends Error {}

/**
 * @deprecated Use the standalone AI Search Workers binding instead.
 * See https://developers.cloudflare.com/ai-search/usage/workers-binding/
 */
export interface AutoRAGNotFoundError extends Error {}

/**
 * @deprecated Use the standalone AI Search Workers binding instead.
 * See https://developers.cloudflare.com/ai-search/usage/workers-binding/
 */
export interface AutoRAGUnauthorizedError extends Error {}

/**
 * @deprecated Use the standalone AI Search Workers binding instead.
 * See https://developers.cloudflare.com/ai-search/usage/workers-binding/
 */
export interface AutoRAGNameNotSetError extends Error {}

export type ComparisonFilter = {
  key: string;
  type: 'eq' | 'ne' | 'gt' | 'gte' | 'lt' | 'lte';
  value: string | number | boolean;
};

export type CompoundFilter = {
  type: 'and' | 'or';
  filters: ComparisonFilter[];
};

/**
 * @deprecated Use the standalone AI Search Workers binding instead.
 * See https://developers.cloudflare.com/ai-search/usage/workers-binding/
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
 * @deprecated Use the standalone AI Search Workers binding instead.
 * See https://developers.cloudflare.com/ai-search/usage/workers-binding/
 */
export type AutoRagAiSearchRequest = AutoRagSearchRequest & {
  stream?: boolean;
  system_prompt?: string;
};

/**
 * @deprecated Use the standalone AI Search Workers binding instead.
 * See https://developers.cloudflare.com/ai-search/usage/workers-binding/
 */
export type AutoRagAiSearchRequestStreaming = Omit<
  AutoRagAiSearchRequest,
  'stream'
> & {
  stream: true;
};

/**
 * @deprecated Use the standalone AI Search Workers binding instead.
 * See https://developers.cloudflare.com/ai-search/usage/workers-binding/
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
 * @deprecated Use the standalone AI Search Workers binding instead.
 * See https://developers.cloudflare.com/ai-search/usage/workers-binding/
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
 * @deprecated Use the standalone AI Search Workers binding instead.
 * See https://developers.cloudflare.com/ai-search/usage/workers-binding/
 */
export type AutoRagAiSearchResponse = AutoRagSearchResponse & {
  response: string;
};

/**
 * @deprecated Use the standalone AI Search Workers binding instead.
 * See https://developers.cloudflare.com/ai-search/usage/workers-binding/
 */
export declare abstract class AutoRAG {
  /**
   * @deprecated Use the standalone AI Search Workers binding instead.
   * See https://developers.cloudflare.com/ai-search/usage/workers-binding/
   */
  list(): Promise<AutoRagListResponse>;

  /**
   * @deprecated Use the standalone AI Search Workers binding instead.
   * See https://developers.cloudflare.com/ai-search/usage/workers-binding/
   */
  search(params: AutoRagSearchRequest): Promise<AutoRagSearchResponse>;

  /**
   * @deprecated Use the standalone AI Search Workers binding instead.
   * See https://developers.cloudflare.com/ai-search/usage/workers-binding/
   */
  aiSearch(params: AutoRagAiSearchRequestStreaming): Promise<Response>;

  /**
   * @deprecated Use the standalone AI Search Workers binding instead.
   * See https://developers.cloudflare.com/ai-search/usage/workers-binding/
   */
  aiSearch(params: AutoRagAiSearchRequest): Promise<AutoRagAiSearchResponse>;

  /**
   * @deprecated Use the standalone AI Search Workers binding instead.
   * See https://developers.cloudflare.com/ai-search/usage/workers-binding/
   */
  aiSearch(
    params: AutoRagAiSearchRequest
  ): Promise<AutoRagAiSearchResponse | Response>;
}
