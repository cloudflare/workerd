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
  cacheTtl?: number;
  skipCache?: boolean;
  metadata?: Record<string, number | string | boolean | null | bigint>;
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
      'http://workers-binding.ai/run?version=3',
      fetchOptions
    );

    this.lastRequestId = res.headers.get('cf-ai-req-id');

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
}

export default function makeBinding(env: { fetcher: Fetcher }): Ai {
  return new Ai(env.fetcher);
}
