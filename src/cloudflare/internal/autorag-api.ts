// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

interface Fetcher {
  fetch: typeof fetch;
}

export class AutoRAGInternalError extends Error {
  public constructor(message: string, name = 'AutoRAGInternalError') {
    super(message);
    this.name = name;
  }
}

export class AutoRAGNotFoundError extends Error {
  public constructor(message: string, name = 'AutoRAGNotFoundError') {
    super(message);
    this.name = name;
  }
}

export class AutoRAGUnauthorizedError extends Error {
  public constructor(message: string, name = 'AutoRAGUnauthorizedError') {
    super(message);
    this.name = name;
  }
}

async function parseError(
  res: Response,
  defaultMsg = 'Internal Error',
  errorCls = AutoRAGInternalError
): Promise<Error> {
  const content = await res.text();

  try {
    const parsedContent = JSON.parse(content) as {
      errors: { message: string }[];
    };

    return new errorCls(parsedContent.errors.at(0)?.message || defaultMsg);
  } catch {
    return new AutoRAGInternalError(content);
  }
}

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

export type AutoRagAiSearchResponse = AutoRagSearchResponse & {
  response: string;
};

export class AutoRAG {
  readonly #fetcher: Fetcher;
  readonly #autoragId: string;

  public constructor(fetcher: Fetcher, autoragId: string) {
    this.#fetcher = fetcher;
    this.#autoragId = autoragId;
  }

  public async search(
    params: AutoRagSearchRequest
  ): Promise<AutoRagSearchResponse> {
    const res = await this.#fetcher.fetch(
      `https://workers-binding.ai/autorag/rags/${this.#autoragId}/search`,
      {
        method: 'POST',
        body: JSON.stringify(params),
        headers: {
          'content-type': 'application/json',
        },
      }
    );

    if (!res.ok) {
      if (res.status === 401) {
        throw await parseError(
          res,
          "You don't have access to this AutoRAG",
          AutoRAGUnauthorizedError
        );
      } else if (res.status === 404) {
        throw await parseError(res, 'AutoRAG not found', AutoRAGNotFoundError);
      }
      throw await parseError(res);
    }

    const data = (await res.json()) as { result: AutoRagSearchResponse };

    return data.result;
  }

  public async aiSearch(
    params: AutoRagSearchRequest
  ): Promise<AutoRagAiSearchResponse> {
    const res = await this.#fetcher.fetch(
      `https://workers-binding.ai/autorag/rags/${this.#autoragId}/ai-search`,
      {
        method: 'POST',
        body: JSON.stringify(params),
        headers: {
          'content-type': 'application/json',
        },
      }
    );

    if (!res.ok) {
      if (res.status === 401) {
        throw await parseError(
          res,
          "You don't have access to this AutoRAG",
          AutoRAGUnauthorizedError
        );
      } else if (res.status === 404) {
        throw await parseError(res, 'AutoRAG not found', AutoRAGNotFoundError);
      }
      throw await parseError(res);
    }

    const data = (await res.json()) as { result: AutoRagAiSearchResponse };

    return data.result;
  }
}
