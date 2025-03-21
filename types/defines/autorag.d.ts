export interface AutoRAGInternalError extends Error {}
export interface AutoRAGNotFoundError extends Error {}
export interface AutoRAGUnauthorizedError extends Error {}

export type AutoRagSearchRequest = {
  query: string;
  max_num_results?: number;
  ranking_options?: {
    ranker?: string;
    score_threshold?: number;
  };
  rewrite_query?: boolean;
};

export type AutoRagSearchResponse = {
  object: "vector_store.search_results.page";
  search_query: string;
  data: {
    file_id: string;
    filename: string;
    score: number;
    attributes: Record<string, string | number | boolean | null>;
    content: {
      type: "text";
      text: string;
    }[];
  }[];
  has_more: boolean;
  next_page: string | null;
};

export type AutoRagAiSearchResponse = AutoRagSearchResponse & {
  response: string;
};

export declare abstract class AutoRAG {
  search(params: AutoRagSearchRequest): Promise<AutoRagSearchResponse>;
  aiSearch(params: AutoRagSearchRequest): Promise<AutoRagAiSearchResponse>;
}
