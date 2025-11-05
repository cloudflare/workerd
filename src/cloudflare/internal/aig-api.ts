// Copyright (c) 2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

interface Fetcher {
  fetch: typeof fetch;
}

type GatewayRetries = {
  maxAttempts?: 1 | 2 | 3 | 4 | 5;
  retryDelayMs?: number;
  backoff?: 'constant' | 'linear' | 'exponential';
};

export type GatewayOptions = {
  id: string;
  cacheKey?: string;
  cacheTtl?: number;
  skipCache?: boolean;
  metadata?: Record<string, number | string | boolean | null | bigint>;
  collectLog?: boolean;
  eventId?: string;
  requestTimeoutMs?: number;
  retries?: GatewayRetries;
};

export type UniversalGatewayOptions = Omit<GatewayOptions, 'id'> & {
  /**
   ** @deprecated
   */
  id?: string;
};

export type AiGatewayPatchLog = {
  score?: number | null;
  feedback?: -1 | 1 | null;
  metadata?: Record<string, number | string | boolean | null | bigint> | null;
};

export type AiGatewayLog = {
  id: string;
  provider: string;
  model: string;
  model_type?: string;
  path: string;
  duration: number;
  request_type?: string;
  request_content_type?: string;
  status_code: number;
  response_content_type?: string;
  success: boolean;
  cached: boolean;
  tokens_in?: number;
  tokens_out?: number;
  metadata?: Record<string, number | string | boolean | null | bigint>;
  step?: number;
  cost?: number;
  custom_cost?: boolean;
  request_size: number;
  request_head?: string;
  request_head_complete: boolean;
  response_size: number;
  response_head?: string;
  response_head_complete: boolean;
  created_at: Date;
};

export type AIGatewayProviders =
  | 'workers-ai'
  | 'anthropic'
  | 'aws-bedrock'
  | 'azure-openai'
  | 'google-vertex-ai'
  | 'huggingface'
  | 'openai'
  | 'perplexity-ai'
  | 'replicate'
  | 'groq'
  | 'cohere'
  | 'google-ai-studio'
  | 'mistral'
  | 'grok'
  | 'openrouter'
  | 'deepseek'
  | 'cerebras'
  | 'cartesia'
  | 'elevenlabs'
  | 'adobe-firefly';

export type AIGatewayHeaders = {
  'cf-aig-metadata':
    | Record<string, number | string | boolean | null | bigint>
    | string;
  'cf-aig-custom-cost':
    | { per_token_in?: number; per_token_out?: number }
    | { total_cost?: number }
    | string;
  'cf-aig-cache-ttl': number | string;
  'cf-aig-skip-cache': boolean | string;
  'cf-aig-cache-key': string;
  'cf-aig-event-id': string;
  'cf-aig-request-timeout': number | string;
  'cf-aig-max-attempts': number | string;
  'cf-aig-retry-delay': number | string;
  'cf-aig-backoff': string;
  'cf-aig-collect-log': boolean | string;
  Authorization: string;
  'Content-Type': string;
  [key: string]: string | number | boolean | object;
};

export type AIGatewayUniversalRequest = {
  provider: AIGatewayProviders | string; // eslint-disable-line
  endpoint: string;
  headers: Partial<AIGatewayHeaders>;
  query: unknown;
};

export class AiGatewayInternalError extends Error {
  constructor(message: string) {
    super(message);
    this.name = 'AiGatewayInternalError';
  }
}

export class AiGatewayLogNotFound extends Error {
  constructor(message: string) {
    super(message);
    this.name = 'AiGatewayLogNotFound';
  }
}

async function parseError(
  res: Response,
  defaultMsg = 'Internal Error',
  errorCls = AiGatewayInternalError
): Promise<Error> {
  const content = await res.text();

  try {
    const parsedContent = JSON.parse(content) as {
      errors: { message: string }[];
    };

    return new errorCls(parsedContent.errors.at(0)?.message || defaultMsg);
  } catch {
    return new AiGatewayInternalError(content);
  }
}

export class AiGateway {
  readonly #fetcher: Fetcher;
  readonly #gatewayId: string;

  constructor(fetcher: Fetcher, gatewayId: string) {
    this.#fetcher = fetcher;
    this.#gatewayId = gatewayId;
  }

  // eslint-disable-next-line
  async getUrl(provider?: AIGatewayProviders | string): Promise<string> {
    const res = await this.#fetcher.fetch(
      `https://workers-binding.ai/ai-gateway/gateways/${this.#gatewayId}/url/${provider ?? 'universal'}`,
      { method: 'GET' }
    );

    if (!res.ok) {
      throw await parseError(res);
    }

    const data = (await res.json()) as { result: { url: string } };

    return data.result.url;
  }

  async getLog(logId: string): Promise<AiGatewayLog> {
    const res = await this.#fetcher.fetch(
      `https://workers-binding.ai/ai-gateway/gateways/${this.#gatewayId}/logs/${logId}`,
      {
        method: 'GET',
      }
    );

    switch (res.status) {
      case 200: {
        const data = (await res.json()) as { result: AiGatewayLog };

        return {
          ...data.result,
          created_at: new Date(data.result.created_at),
        };
      }
      case 404: {
        throw await parseError(res, 'Log Not Found', AiGatewayLogNotFound);
      }
      default: {
        throw await parseError(res);
      }
    }
  }

  async patchLog(logId: string, data: AiGatewayPatchLog): Promise<void> {
    const res = await this.#fetcher.fetch(
      `https://workers-binding.ai/ai-gateway/gateways/${this.#gatewayId}/logs/${logId}`,
      {
        method: 'PATCH',
        body: JSON.stringify(data),
        headers: {
          'content-type': 'application/json',
        },
      }
    );

    switch (res.status) {
      case 200: {
        return;
      }
      case 404: {
        throw await parseError(res, 'Log Not Found', AiGatewayLogNotFound);
      }
      default: {
        throw await parseError(res);
      }
    }
  }

  run(
    data: AIGatewayUniversalRequest | AIGatewayUniversalRequest[],
    options?: {
      gateway?: UniversalGatewayOptions;
      extraHeaders?: Record<string, string>;
    }
  ): Promise<Response> {
    const input = Array.isArray(data) ? data : [data];

    const headers = this.#getHeadersFromOptions(
      options?.gateway,
      options?.extraHeaders
    );

    // Convert header values to string
    for (const req of input) {
      for (const [k, v] of Object.entries(req.headers)) {
        if (typeof v === 'number' || typeof v === 'boolean') {
          req.headers[k] = v.toString();
          // eslint-disable-next-line
        } else if (typeof v === 'object' && v != null) {
          req.headers[k] = JSON.stringify(v);
        }
      }
    }

    return this.#fetcher.fetch(
      `https://workers-binding.ai/ai-gateway/universal/run/${this.#gatewayId}`,
      {
        method: 'POST',
        body: JSON.stringify(input),
        headers: headers,
      }
    );
  }

  #getHeadersFromOptions(
    options?: UniversalGatewayOptions,
    extraHeaders?: Record<string, string>
  ): Headers {
    const headers = new Headers();
    headers.set('content-type', 'application/json');

    if (options) {
      if (options.skipCache !== undefined) {
        headers.set('cf-aig-skip-cache', options.skipCache ? 'true' : 'false');
      }

      if (options.cacheTtl) {
        headers.set('cf-aig-cache-ttl', options.cacheTtl.toString());
      }

      if (options.metadata) {
        headers.set('cf-aig-metadata', JSON.stringify(options.metadata));
      }

      if (options.cacheKey) {
        headers.set('cf-aig-cache-key', options.cacheKey);
      }

      if (options.collectLog !== undefined) {
        headers.set(
          'cf-aig-collect-log',
          options.collectLog ? 'true' : 'false'
        );
      }

      if (options.eventId !== undefined) {
        headers.set('cf-aig-event-id', options.eventId);
      }

      if (options.requestTimeoutMs !== undefined) {
        headers.set(
          'cf-aig-request-timeout',
          options.requestTimeoutMs.toString()
        );
      }

      if (options.retries !== undefined) {
        if (options.retries.maxAttempts !== undefined) {
          headers.set(
            'cf-aig-max-attempts',
            options.retries.maxAttempts.toString()
          );
        }
        if (options.retries.retryDelayMs !== undefined) {
          headers.set(
            'cf-aig-retry-delay',
            options.retries.retryDelayMs.toString()
          );
        }
        if (options.retries.backoff !== undefined) {
          headers.set('cf-aig-backoff', options.retries.backoff);
        }
      }
    }

    if (extraHeaders) {
      for (const [key, value] of Object.entries(extraHeaders)) {
        headers.set(key, value);
      }
    }

    return headers;
  }
}
