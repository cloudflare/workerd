// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

interface Fetcher {
  fetch: typeof fetch;
}

// Error classes
export class AiSearchInternalError extends Error {
  constructor(message: string, name = 'AiSearchInternalError') {
    super(message);
    this.name = name;
  }
}

export class AiSearchNotFoundError extends Error {
  constructor(message: string, name = 'AiSearchNotFoundError') {
    super(message);
    this.name = name;
  }
}

export class AiSearchNameNotSetError extends Error {
  constructor(message: string, name = 'AiSearchNameNotSetError') {
    super(message);
    this.name = name;
  }
}

async function parseError(
  res: Response,
  defaultMsg = 'Internal Error',
  errorCls = AiSearchInternalError
): Promise<Error> {
  const content = await res.text();

  try {
    const parsedContent = JSON.parse(content) as {
      errors: { message: string }[];
    };

    return new errorCls(parsedContent.errors.at(0)?.message || defaultMsg);
  } catch {
    return new AiSearchInternalError(content);
  }
}

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

// V2 API Type Definitions

export type AiSearchSearchRequest = {
  messages: Array<{
    role: 'system' | 'developer' | 'user' | 'assistant' | 'tool';
    content: string | null;
  }>;
  ai_search_options?: {
    retrieval?: {
      retrieval_type?: 'vector' | 'keyword' | 'hybrid';
      match_threshold?: number; // 0-1, default 0.4
      max_num_results?: number; // 1-50, default 10
      filters?: CompoundFilter | ComparisonFilter;
      context_expansion?: number; // 0-3, default 0
      [key: string]: unknown; // Future extensibility
    };
    query_rewrite?: {
      enabled?: boolean;
      model?: string;
      rewrite_prompt?: string;
      [key: string]: unknown; // Future extensibility
    };
    reranking?: {
      enabled?: boolean; // default false
      model?: '@cf/baai/bge-reranker-base' | '';
      match_threshold?: number; // 0-1, default 0.4
      [key: string]: unknown; // Future extensibility
    };
    [key: string]: unknown; // Future extensibility - allows new top-level options
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
      [key: string]: unknown; // Future extensibility
    };
    query_rewrite?: {
      enabled?: boolean;
      model?: string;
      rewrite_prompt?: string;
      [key: string]: unknown; // Future extensibility
    };
    reranking?: {
      enabled?: boolean;
      model?: '@cf/baai/bge-reranker-base' | '';
      match_threshold?: number;
      [key: string]: unknown; // Future extensibility
    };
    [key: string]: unknown; // Future extensibility
  };
  [key: string]: unknown; // additionalProperties allowed for future extensibility
};

export type AiSearchSearchResponse = {
  search_query: string;
  chunks: Array<{
    id: string;
    type: string;
    score: number; // 0-1
    text: string;
    item: {
      timestamp?: number;
      key: string;
      metadata?: Record<string, unknown>;
    };
    scoring_details?: {
      keyword_score?: number; // 0-1
      vector_score?: number; // 0-1
    };
  }>;
};

export type AiSearchListResponse = Array<{
  id: string;
  internal_id?: string;
  account_id?: string;
  account_tag?: string;
  enable?: boolean; // default true
  type?: 'r2' | 'web-crawler';
  source?: string;
  [key: string]: unknown; // Allow additional fields from API
}>;

export type AiSearchConfig = {
  id: string; // 1-32 chars, pattern: ^[a-z0-9_]+(?:-[a-z0-9_]+)*$
  type: 'r2' | 'web-crawler';
  source: string;
  source_params?: object;
  token_id?: string; // UUID format
  ai_gateway_id?: string;
  rewrite_query?: boolean; // default false
  reranking?: boolean; // default false
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

// Account-level service
export class AiSearchAccountService {
  readonly #fetcher: Fetcher;

  constructor(fetcher: Fetcher) {
    this.#fetcher = fetcher;
  }

  async list(): Promise<AiSearchListResponse> {
    const res = await this.#fetcher.fetch(
      'https://workers-binding.ai/ai-search/instances',
      {
        method: 'GET',
        headers: {
          'content-type': 'application/json',
        },
      }
    );

    if (!res.ok) {
      throw await parseError(res);
    }

    const data = (await res.json()) as { result: AiSearchListResponse };
    return data.result;
  }

  get(name: string): AiSearchInstanceService {
    return new AiSearchInstanceService(this.#fetcher, name);
  }

  async create(config: AiSearchConfig): Promise<AiSearchInstanceService> {
    const res = await this.#fetcher.fetch(
      'https://workers-binding.ai/ai-search/instances',
      {
        method: 'POST',
        body: JSON.stringify(config),
        headers: {
          'content-type': 'application/json',
        },
      }
    );

    if (!res.ok) {
      throw await parseError(res);
    }

    return new AiSearchInstanceService(this.#fetcher, config.id);
  }
}

// Instance-level service
export class AiSearchInstanceService {
  readonly #fetcher: Fetcher;
  readonly #instanceId: string;

  constructor(fetcher: Fetcher, instanceId: string) {
    this.#fetcher = fetcher;
    this.#instanceId = instanceId;
  }

  async search(params: AiSearchSearchRequest): Promise<AiSearchSearchResponse> {
    if (!this.#instanceId) {
      throw new AiSearchNameNotSetError('AI Search instance name not defined');
    }

    const res = await this.#fetcher.fetch(
      `https://workers-binding.ai/ai-search/instances/${this.#instanceId}/search`,
      {
        method: 'POST',
        body: JSON.stringify(params),
        headers: {
          'content-type': 'application/json',
        },
      }
    );

    if (!res.ok) {
      if (res.status === 404) {
        throw await parseError(
          res,
          'AI Search instance not found',
          AiSearchNotFoundError
        );
      }
      throw await parseError(res);
    }

    const data = (await res.json()) as { result: AiSearchSearchResponse };
    return data.result;
  }

  async chatCompletions(
    params: AiSearchChatCompletionsRequest
  ): Promise<Response | object> {
    if (!this.#instanceId) {
      throw new AiSearchNameNotSetError('AI Search instance name not defined');
    }

    const res = await this.#fetcher.fetch(
      `https://workers-binding.ai/ai-search/instances/${this.#instanceId}/chat/completions`,
      {
        method: 'POST',
        body: JSON.stringify(params),
        headers: {
          'content-type': 'application/json',
        },
      }
    );

    if (!res.ok) {
      if (res.status === 404) {
        throw await parseError(
          res,
          'AI Search instance not found',
          AiSearchNotFoundError
        );
      }
      throw await parseError(res);
    }

    // Return Response if streaming, otherwise parse JSON
    if (params.stream === true) {
      return res;
    }

    const data = (await res.json()) as { result: object };
    return data.result;
  }

  async delete(): Promise<void> {
    if (!this.#instanceId) {
      throw new AiSearchNameNotSetError('AI Search instance name not defined');
    }

    const res = await this.#fetcher.fetch(
      `https://workers-binding.ai/ai-search/instances/${this.#instanceId}`,
      {
        method: 'DELETE',
        headers: {
          'content-type': 'application/json',
        },
      }
    );

    if (!res.ok) {
      if (res.status === 404) {
        throw await parseError(
          res,
          'AI Search instance not found',
          AiSearchNotFoundError
        );
      }
      throw await parseError(res);
    }
  }
}
