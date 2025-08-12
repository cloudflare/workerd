export interface AutoRAGInternalError extends Error {}
export interface AutoRAGNotFoundError extends Error {}
export interface AutoRAGUnauthorizedError extends Error {}
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
export type AutoRagSearchRequest = {
  query: string;
  filters?: CompoundFilter | ComparisonFilter;
  max_num_results?: number;
  ranking_options?: {
    ranker?: string;
    score_threshold?: number;
  };
  rewrite_query?: boolean;
};
export type AutoRagAiSearchRequest = AutoRagSearchRequest & {
  stream?: boolean;
  system_prompt?: string;
};
export type AutoRagAiSearchRequestStreaming = Omit<
  AutoRagAiSearchRequest,
  'stream'
> & {
  stream: true;
};
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

export type AutoRagListResponse = {
  id: string;
  enable: boolean;
  type: string;
  source: string;
  vectorize_name: string;
  paused: boolean;
  status: string;
}[];

export type AutoRagAiSearchResponse = AutoRagSearchResponse & {
  response: string;
};

export declare abstract class AutoRAG {
  list(): Promise<AutoRagListResponse>;
  search(params: AutoRagSearchRequest): Promise<AutoRagSearchResponse>;
  aiSearch(params: AutoRagAiSearchRequestStreaming): Promise<Response>;
  aiSearch(params: AutoRagAiSearchRequest): Promise<AutoRagAiSearchResponse>;
  aiSearch(
    params: AutoRagAiSearchRequest
  ): Promise<AutoRagAiSearchResponse | Response>;
}
