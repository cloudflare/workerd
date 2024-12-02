// Copyright (c) 2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

interface Fetcher {
  fetch: typeof fetch;
}

export type GatewayOptions = {
  id: string;
  cacheKey?: string;
  cacheTtl?: number;
  skipCache?: boolean;
  metadata?: Record<string, number | string | boolean | null | bigint>;
  collectLog?: boolean;
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
  | 'openrouter';

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
  public constructor(message: string) {
    super(message);
    this.name = 'AiGatewayInternalError';
  }
}

export class AiGatewayLogNotFound extends Error {
  public constructor(message: string) {
    super(message);
    this.name = 'AiGatewayLogNotFound';
  }
}

export class AiGateway {
  readonly #fetcher: Fetcher;
  readonly #gatewayId: string;

  public constructor(fetcher: Fetcher, gatewayId: string) {
    this.#fetcher = fetcher;
    this.#gatewayId = gatewayId;
  }

  public async getLog(logId: string): Promise<AiGatewayLog> {
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
        const data = (await res.json()) as { errors: { message: string }[] };

        throw new AiGatewayLogNotFound(
          data.errors[0]?.message || 'Log Not Found'
        );
      }
      default: {
        const data = (await res.json()) as { errors: { message: string }[] };

        throw new AiGatewayInternalError(
          data.errors[0]?.message || 'Internal Error'
        );
      }
    }
  }

  public async patchLog(logId: string, data: AiGatewayPatchLog): Promise<void> {
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
        const data = (await res.json()) as { errors: { message: string }[] };

        throw new AiGatewayLogNotFound(
          data.errors[0]?.message || 'Log Not Found'
        );
      }
      default: {
        const data = (await res.json()) as { errors: { message: string }[] };

        throw new AiGatewayInternalError(
          data.errors[0]?.message || 'Internal Error'
        );
      }
    }
  }

  public run(
    data: AIGatewayUniversalRequest | AIGatewayUniversalRequest[]
  ): Promise<Response> {
    const input = Array.isArray(data) ? data : [data];

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
        headers: {
          'content-type': 'application/json',
        },
      }
    );
  }
}
