// Copyright (c) 2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

interface Fetcher {
  fetch: typeof fetch;
}

interface AiError {
  internalCode: number;
  message: string;
  name: string;
  description: string;
}

export type SessionOptions = {
  // Deprecated, do not use this
  extraHeaders?: object;
};

export type GatewayOptions = {
  id: string;
  cacheKey?: string;
  cacheTtl?: number;
  skipCache?: boolean;
  metadata?: Record<string, number | string | boolean | null | bigint>;
  collectLog?: boolean;
};

export type AiOptions = {
  gateway?: GatewayOptions;

  prefix?: string;
  extraHeaders?: object;
  /*
   * @deprecated this option is deprecated, do not use this
   */
  sessionOptions?: SessionOptions;
};

export class InferenceUpstreamError extends Error {
  public constructor(message: string, name = 'InferenceUpstreamError') {
    super(message);
    this.name = name;
  }
}

export class Ai {
  private readonly fetcher: Fetcher;

  /*
   * @deprecated this option is deprecated, do not use this
   */
  // @ts-expect-error this option is deprecated, do not use this
  private logs: Array<string> = [];
  private options: AiOptions = {};
  public lastRequestId: string | null = null;
  public aiGatewayLogId: string | null = null;

  public constructor(fetcher: Fetcher) {
    this.fetcher = fetcher;
  }

  public async fetch(
    input: RequestInfo | URL,
    init?: RequestInit
  ): Promise<Response> {
    return this.fetcher.fetch(input, init);
  }

  public async run(
    model: string,
    inputs: Record<string, object>,
    options: AiOptions = {}
  ): Promise<Response | ReadableStream<Uint8Array> | object | null> {
    this.options = options;
    this.lastRequestId = '';

    // This removes some unwanted options from getting sent in the body
    const cleanedOptions = (({
      prefix,
      extraHeaders,
      sessionOptions,
      ...object
    }): object => object)(this.options);

    const body = JSON.stringify({
      inputs,
      options: cleanedOptions,
    });

    const fetchOptions = {
      method: 'POST',
      body: body,
      headers: {
        ...(this.options.sessionOptions?.extraHeaders || {}),
        ...(this.options.extraHeaders || {}),
        'content-type': 'application/json',
        'cf-consn-sdk-version': '2.0.0',
        'cf-consn-model-id': `${this.options.prefix ? `${this.options.prefix}:` : ''}${model}`,
      },
    };

    const res = await this.fetcher.fetch(
      'https://workers-binding.ai/run?version=3',
      fetchOptions
    );

    this.lastRequestId = res.headers.get('cf-ai-req-id');
    this.aiGatewayLogId = res.headers.get('cf-aig-log-id');

    if (inputs['stream']) {
      if (!res.ok) {
        throw await this._parseError(res);
      }

      return res.body;
    } else {
      if (!res.ok || !res.body) {
        throw await this._parseError(res);
      }

      const contentType = res.headers.get('content-type');

      if (contentType === 'application/json') {
        return (await res.json()) as object;
      }

      return res.body;
    }
  }

  /*
   * @deprecated this method is deprecated, do not use this
   */
  public getLogs(): string[] {
    return [];
  }

  private async _parseError(res: Response): Promise<InferenceUpstreamError> {
    const content = await res.text();

    try {
      const parsedContent = JSON.parse(content) as AiError;
      return new InferenceUpstreamError(
        `${parsedContent.internalCode}: ${parsedContent.description}`,
        parsedContent.name
      );
    } catch {
      return new InferenceUpstreamError(content);
    }
  }

  public gateway(gatewayId: string): AiGateway {
    return new AiGateway(this.fetcher, gatewayId);
  }
}

//
// Ai Gateway
//

export type AiGatewayPatchLog = {
  score?: number | null;
  feedback?: -1 | 1 | '-1' | '1' | null;
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
  private readonly fetcher: Fetcher;
  private readonly gatewayId: string;

  public constructor(fetcher: Fetcher, gatewayId: string) {
    this.fetcher = fetcher;
    this.gatewayId = gatewayId;
  }

  public async getLog(logId: string): Promise<AiGatewayLog> {
    const res = await this.fetcher.fetch(
      `https://workers-binding.ai/ai-gateway/gateways/${this.gatewayId}/logs/${logId}`,
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
    const res = await this.fetcher.fetch(
      `https://workers-binding.ai/ai-gateway/gateways/${this.gatewayId}/logs/${logId}`,
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
}

export default function makeBinding(env: { fetcher: Fetcher }): Ai {
  return new Ai(env.fetcher);
}
