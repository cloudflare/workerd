// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { AiGateway, type GatewayOptions } from 'cloudflare-internal:aig-api';
import { AutoRAG } from 'cloudflare-internal:autorag-api';
import base64 from 'cloudflare-internal:base64';

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

export type AiInputReadableStream = {
  body: ReadableStream;
  contentType: string;
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
  constructor(message: string, name = 'InferenceUpstreamError') {
    super(message);
    this.name = name;
  }
}

export class AiInternalError extends Error {
  constructor(message: string, name = 'AiInternalError') {
    super(message);
    this.name = name;
  }
}

async function blobToBase64(blob: Blob): Promise<string> {
  return base64.encodeArrayToString(await blob.arrayBuffer());
}

// TODO: merge this function with the one with images-api.ts
function isReadableStream(obj: unknown): obj is ReadableStream {
  return !!(
    obj &&
    typeof obj === 'object' &&
    'getReader' in obj &&
    typeof obj.getReader === 'function'
  );
}

/**
 * Find keys in inputs that have a ReadableStream
 * */
function findReadableStreamKeys(
  inputs: Record<string, unknown>
): Array<string> {
  const readableStreamKeys: Array<string> = [];

  for (const [key, value] of Object.entries(inputs)) {
    // Check if value has a body property that's a ReadableStream
    const hasReadableStreamBody =
      value &&
      typeof value === 'object' &&
      'body' in value &&
      isReadableStream(value.body);

    if (hasReadableStreamBody || isReadableStream(value)) {
      readableStreamKeys.push(key);
    }
  }

  return readableStreamKeys;
}

export class Ai {
  // TODO(soon): Can we use the # syntax here?
  // eslint-disable-next-line no-restricted-syntax
  private readonly fetcher: Fetcher;

  /*
   * @deprecated this option is deprecated, do not use this
   */
  // TODO(soon): Can we use the # syntax here?
  // @ts-expect-error this option is deprecated, do not use this
  // eslint-disable-next-line no-restricted-syntax
  private logs: Array<string> = [];
  // TODO(soon): Can we use the # syntax here?
  // eslint-disable-next-line no-restricted-syntax
  private options: AiOptions = {};
  lastRequestId: string | null = null;
  aiGatewayLogId: string | null = null;
  lastRequestHttpStatusCode: number | null = null;
  lastRequestInternalStatusCode: number | null = null;

  constructor(fetcher: Fetcher) {
    this.fetcher = fetcher;
  }

  async fetch(input: RequestInfo | URL, init?: RequestInit): Promise<Response> {
    return this.fetcher.fetch(input, init);
  }

  async run(
    model: string,
    inputs: Record<string, unknown>,
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

    let res: Response;
    /**
     * Inputs that contain a ReadableStream which will be sent directly to
     * the fetcher object along with other keys parsed as a query parameters
     * */
    const streamKeys = findReadableStreamKeys(inputs);

    if (streamKeys.length === 0) {
      // Treat inputs as regular JS objects
      const body = JSON.stringify({
        inputs,
        options: cleanedOptions,
      });

      const fetchOptions = {
        method: 'POST',
        body: body,
        headers: {
          ...this.options.sessionOptions?.extraHeaders,
          ...this.options.extraHeaders,
          'content-type': 'application/json',
          'cf-consn-sdk-version': '2.0.0',
          'cf-consn-model-id': `${this.options.prefix ? `${this.options.prefix}:` : ''}${model}`,
        },
      };

      let endpointUrl = 'https://workers-binding.ai/run?version=3';
      if (options.gateway?.id) {
        endpointUrl = 'https://workers-binding.ai/ai-gateway/run?version=3';
      }

      res = await this.fetcher.fetch(endpointUrl, fetchOptions);
    } else if (streamKeys.length > 1) {
      throw new AiInternalError(
        `Multiple ReadableStreams are not supported. Found streams in keys: [${streamKeys.join(', ')}]`
      );
    } else {
      const streamKey = streamKeys[0] ?? '';
      const stream = streamKey ? inputs[streamKey] : null;
      const body = (stream as AiInputReadableStream).body;
      const contentType = (stream as AiInputReadableStream).contentType;

      if (options.gateway?.id) {
        throw new AiInternalError(
          'AI Gateway does not support ReadableStreams yet.'
        );
      }

      // Make sure user has supplied the Content-Type
      // This allows AI binding to treat the ReadableStream correctly
      if (!contentType) {
        throw new AiInternalError(
          'Content-Type is required with ReadableStream inputs'
        );
      }

      // Pass single ReadableStream in request body
      const fetchOptions = {
        method: 'POST',
        body: body,
        headers: {
          ...this.options.sessionOptions?.extraHeaders,
          ...this.options.extraHeaders,
          'content-type': contentType,
          'cf-consn-sdk-version': '2.0.0',
          'cf-consn-model-id': `${this.options.prefix ? `${this.options.prefix}:` : ''}${model}`,
        },
      };

      // Fetch the additional input params
      const { [streamKey]: streamInput, ...userInputs } = inputs;

      // Construct query params
      // Append inputs with ai.run options that are passed to the inference request
      const query = {
        ...cleanedOptions,
        version: '3',
        userInputs: JSON.stringify({ ...userInputs }),
      };
      const aiEndpoint = new URL('https://workers-binding.ai/run');
      for (const [key, value] of Object.entries(query)) {
        aiEndpoint.searchParams.set(key, value);
      }

      res = await this.fetcher.fetch(aiEndpoint, fetchOptions);
    }

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
  getLogs(): string[] {
    return [];
  }

  // TODO(soon): Can we use the # syntax here?
  // eslint-disable-next-line no-restricted-syntax
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

  async models(
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

  async toMarkdown(
    files: { name: string; blob: Blob }[],
    options?: { gateway?: GatewayOptions; extraHeaders?: object }
  ): Promise<ConversionResponse[]>;
  async toMarkdown(
    files: {
      name: string;
      blob: Blob;
    },
    options?: { gateway?: GatewayOptions; extraHeaders?: object }
  ): Promise<ConversionResponse>;
  async toMarkdown(
    files: { name: string; blob: Blob } | { name: string; blob: Blob }[],
    options?: { gateway?: GatewayOptions; extraHeaders?: object }
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
        ...options?.extraHeaders,
        'content-type': 'application/json',
      },
    };

    const endpointUrl =
      'https://workers-binding.ai/to-everything/markdown/transformer';

    const res = await this.fetcher.fetch(endpointUrl, fetchOptions);

    if (!res.ok) {
      const content = await res.text();
      let parsedContent;

      try {
        parsedContent = JSON.parse(content) as {
          errors: { message: string }[];
        };
      } catch {
        throw new AiInternalError(content);
      }

      throw new AiInternalError(
        parsedContent.errors.at(0)?.message || 'Internal Error'
      );
    }

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

  gateway(gatewayId: string): AiGateway {
    return new AiGateway(this.fetcher, gatewayId);
  }

  autorag(autoragId?: string): AutoRAG {
    return new AutoRAG(this.fetcher, autoragId);
  }
}

export default function makeBinding(env: { fetcher: Fetcher }): Ai {
  return new Ai(env.fetcher);
}
