// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { AiGateway, type GatewayOptions } from 'cloudflare-internal:aig-api';

interface Fetcher {
  fetch: typeof fetch;
}

interface AiError {
  internalCode: number;
  message: string;
  name: string;
  description: string;
  errors?: Array<{ code: number; message: string }>;
}

export type SessionOptions = {
  // Deprecated, do not use this
  extraHeaders?: object;
};

export type AiOptions = {
  gateway?: GatewayOptions;
  /** If true it will return a Response object */
  returnRawResponse?: boolean;
  prefix?: string;
  extraHeaders?: object;
  /*
   * @deprecated this option is deprecated, do not use this
   */
  sessionOptions?: SessionOptions;
};

export type ConversionResponse = {
  name: string;
  mimeType: string;
  format: 'markdown';
  tokens: number;
  data: string;
};

export type AiModelsSearchParams = {
  author?: string;
  hide_experimental?: boolean;
  page?: number;
  per_page?: number;
  search?: string;
  source?: number;
  task?: string;
};

export type AiModelsSearchObject = {
  id: string;
  source: number;
  name: string;
  description: string;
  task: {
    id: string;
    name: string;
    description: string;
  };
  tags: string[];
  properties: {
    property_id: string;
    value: string;
  }[];
};

export class InferenceUpstreamError extends Error {
  public constructor(message: string, name = 'InferenceUpstreamError') {
    super(message);
    this.name = name;
  }
}

export class AiInternalError extends Error {
  public constructor(message: string, name = 'AiInternalError') {
    super(message);
    this.name = name;
  }
}

async function blobToBase64(blob: Blob): Promise<string> {
  // TODO(soon): This is better implemented using the node::buffer API
  // but we cannot get to that from here currently. Once the node:buffer
  // API (actually, `node-internal:internal_buffer`) is available to be imported
  // here we should update this code to use it instead.
  const arrayBuffer = await blob.arrayBuffer();
  const uint8Array = new Uint8Array(arrayBuffer);

  let binary = '';
  const chunk = 1024;
  for (let i = 0; i < uint8Array.length; i += chunk) {
    binary += String.fromCharCode.apply(
      null,
      uint8Array.subarray(i, i + chunk) as unknown as number[]
    );
  }

  return btoa(binary);
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
  public lastRequestHttpStatusCode: number | null = null;
  public lastRequestInternalStatusCode: number | null = null;

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

    let endpointUrl = 'https://workers-binding.ai/run?version=3';
    if (options.gateway?.id) {
      endpointUrl = 'https://workers-binding.ai/ai-gateway/run?version=3';
    }

    const res = await this.fetcher.fetch(endpointUrl, fetchOptions);

    this.lastRequestId = res.headers.get('cf-ai-req-id');
    this.aiGatewayLogId = res.headers.get('cf-aig-log-id');
    this.lastRequestHttpStatusCode = res.status;

    if (this.options.returnRawResponse) {
      return res;
    }

    if (!res.ok || !res.body) {
      throw await this._parseError(res);
    }

    const contentType = res.headers.get('content-type');
    if (contentType === 'application/json') {
      return (await res.json()) as object;
    }

    return res.body;
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
      if (parsedContent.internalCode) {
        this.lastRequestInternalStatusCode = parsedContent.internalCode;
        return new InferenceUpstreamError(
          `${parsedContent.internalCode}: ${parsedContent.description}`,
          parsedContent.name
        );
      } else if (
        parsedContent.errors &&
        parsedContent.errors.length > 0 &&
        parsedContent.errors[0]
      ) {
        return new InferenceUpstreamError(
          `${parsedContent.errors[0].code}: ${parsedContent.errors[0].message}`
        );
      } else {
        return new InferenceUpstreamError(content);
      }
    } catch {
      return new InferenceUpstreamError(content);
    }
  }

  public async models(
    params: AiModelsSearchParams = {}
  ): Promise<AiModelsSearchObject[]> {
    const url = new URL('https://workers-binding.ai/ai-api/models/search');

    for (const [key, value] of Object.entries(params)) {
      url.searchParams.set(key, value.toString());
    }

    const res = await this.fetcher.fetch(url, { method: 'GET' });

    switch (res.status) {
      case 200: {
        const data = (await res.json()) as { result: AiModelsSearchObject[] };
        return data.result;
      }
      default: {
        const data = (await res.json()) as { errors: { message: string }[] };

        throw new AiInternalError(data.errors[0]?.message || 'Internal Error');
      }
    }
  }

  public async toMarkdown(
    files: { name: string; blob: Blob }[],
    options?: { gateway?: GatewayOptions }
  ): Promise<ConversionResponse[]>;
  public async toMarkdown(
    files: {
      name: string;
      blob: Blob;
    },
    options?: { gateway?: GatewayOptions }
  ): Promise<ConversionResponse>;
  public async toMarkdown(
    files: { name: string; blob: Blob } | { name: string; blob: Blob }[],
    options?: { gateway?: GatewayOptions }
  ): Promise<ConversionResponse | ConversionResponse[]> {
    const input = Array.isArray(files) ? files : [files];

    const processedFiles = [];
    for (const file of input) {
      processedFiles.push({
        name: file.name,
        mimeType: file.blob.type,
        data: await blobToBase64(file.blob),
      });
    }

    const fetchOptions = {
      method: 'POST',
      body: JSON.stringify({
        files: processedFiles,
        options: options,
      }),
      headers: {
        'content-type': 'application/json',
      },
    };

    const endpointUrl =
      'https://workers-binding.ai/to-everything/markdown/transformer';

    const res = await this.fetcher.fetch(endpointUrl, fetchOptions);

    switch (res.status) {
      case 200: {
        const data = (await res.json()) as { result: ConversionResponse[] };

        if (data.result.length === 0) {
          throw new AiInternalError(
            'Internal Error Converting files into Markdown'
          );
        }

        // If the user sent a list of files, return an array of results, otherwise, return just the first object
        if (Array.isArray(files)) {
          return data.result;
        }

        const obj = data.result.at(0);
        if (!obj) {
          throw new AiInternalError(
            'Internal Error Converting files into Markdown'
          );
        }

        return obj;
      }
      default: {
        const data = (await res.json()) as { errors: { message: string }[] };

        throw new AiInternalError(
          data.errors.at(0)?.message || 'Internal Error'
        );
      }
    }
  }

  public gateway(gatewayId: string): AiGateway {
    return new AiGateway(this.fetcher, gatewayId);
  }
}

export default function makeBinding(env: { fetcher: Fetcher }): Ai {
  return new Ai(env.fetcher);
}
