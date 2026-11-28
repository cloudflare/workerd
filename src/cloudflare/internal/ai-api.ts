// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { AiGateway, type GatewayOptions } from 'cloudflare-internal:aig-api';
import { AutoRAG } from 'cloudflare-internal:autorag-api';
import wrappedBinding from 'cloudflare-internal:wrapped-binding';
import {
  ToMarkdownService,
  type ConversionRequestOptions,
  type ConversionResponse,
  type MarkdownDocument,
} from 'cloudflare-internal:to-markdown-api';

type AiSearchService = object;

const aiBindingExperimental = !!Cloudflare.compatibilityFlags['experimental'];

interface Fetcher {
  fetch: typeof fetch;
  aiSearch: () => AiSearchService;
  websearch: (
    request: AiWebSearchRequest,
    params?: AiWebSearchParams
  ) => Promise<Response>;
  gateway: (gatewayId: string) => AiGateway;
  autorag: (autoragId?: string) => AutoRAG;
  toMarkdown: () => ToMarkdownService;
}

interface AiError {
  internalCode?: number;
  httpCode?: number;
  message?: string;
  name?: string;
  description?: string;
  retryable?: boolean;
  requestId?: string;
  errors?: Array<{ code: number; message: string }>;
}

export type SessionOptions = {
  // Deprecated, do not use this
  extraHeaders?: object;
};

export type AiOptions = {
  gateway?: GatewayOptions;
  websocket?: boolean;
  /** If true it will return a Response object */
  returnRawResponse?: boolean;
  prefix?: string;
  extraHeaders?: object;
  /*
   * @deprecated this option is deprecated, do not use this
   */
  sessionOptions?: SessionOptions;
  signal?: AbortSignal;
};

export type AiRunResult<Result> = {
  result: Result;
  logId: string | null;
  requestId: string | null;
  httpStatus: number;
};

type AiRunOutput = Response | ReadableStream<Uint8Array> | object | null;

export type AiWebSearchRequest = {
  gatewayId: string;
  query: string;
  limit?: number;
  provider?: string;
  byokAlias?: string;
};

type AiWebSearchParams = Pick<AiOptions, 'extraHeaders'>;

type CleanedAiOptions = Omit<
  AiOptions,
  'prefix' | 'extraHeaders' | 'sessionOptions' | 'signal'
>;

export type AiInputReadableStream = {
  body: ReadableStream | FormData;
  contentType: string;
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

export interface InferenceUpstreamErrorDetails {
  code?: number | undefined;
  httpStatus?: number | undefined;
  retryable?: boolean | undefined;
  requestId?: string | undefined;
  logId?: string | undefined;
  description?: string | undefined;
}

export class InferenceUpstreamError extends Error {
  code?: number;
  httpStatus?: number;
  retryable?: boolean;
  requestId?: string;
  logId?: string;
  description?: string;

  constructor(
    message: string,
    name = 'InferenceUpstreamError',
    details?: InferenceUpstreamErrorDetails
  ) {
    super(message);
    this.name = name;
    if (details) {
      if (details.code !== undefined) this.code = details.code;
      if (details.httpStatus !== undefined) {
        this.httpStatus = details.httpStatus;
      }
      if (details.retryable !== undefined) this.retryable = details.retryable;
      if (details.requestId !== undefined) this.requestId = details.requestId;
      if (details.logId !== undefined) this.logId = details.logId;
      if (details.description !== undefined) {
        this.description = details.description;
      }
    }
  }
}

export class AiInternalError extends Error {
  constructor(message: string, name = 'AiInternalError') {
    super(message);
    this.name = name;
  }
}

function isReadableStream(obj: unknown): obj is ReadableStream {
  return obj instanceof ReadableStream;
}

function isFormData(obj: unknown): obj is FormData {
  return obj instanceof FormData;
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
      (isReadableStream(value.body) || isFormData(value.body));

    if (hasReadableStreamBody || isReadableStream(value) || isFormData(value)) {
      readableStreamKeys.push(key);
    }
  }

  return readableStreamKeys;
}

export class Ai extends wrappedBinding.WrappedBinding {
  #fetcher: Fetcher;
  #responseSequence = 0;

  /*
   * @deprecated this option is deprecated, do not use this
   */
  // @ts-expect-error: deprecated var
  // eslint-disable-next-line no-unused-private-class-members
  #logs: Array<string> = [];
  #endpointURL = 'https://workers-binding.ai';
  lastRequestId: string | null = null;
  aiGatewayLogId: string | null = null;
  lastRequestHttpStatusCode: number | null = null;
  lastRequestInternalStatusCode: number | null = null;

  constructor(fetcher: Fetcher) {
    super(fetcher);
    this.#fetcher = fetcher;
  }

  async fetch(input: RequestInfo | URL, init?: RequestInit): Promise<Response> {
    return this.#fetcher.fetch(input, init);
  }

  /**
   * Generate fetch call for JSON inputs
   * */
  async #generateFetch(
    inputs: object,
    options: AiOptions,
    cleanedOptions: CleanedAiOptions,
    model: string
  ): Promise<Response> {
    // Treat inputs as regular JS objects
    const body = JSON.stringify({
      inputs,
      options: cleanedOptions,
    });

    const fetchOptions: RequestInit = {
      method: 'POST',
      body: body,
      headers: {
        ...options.sessionOptions?.extraHeaders,
        ...options.extraHeaders,
        'content-type': 'application/json',
        'cf-consn-sdk-version': '2.0.0',
        'cf-consn-model-id': `${options.prefix ? `${options.prefix}:` : ''}${model}`,
      },
    };
    if (options.signal) {
      fetchOptions.signal = options.signal;
    }

    let endpointUrl = `${this.#endpointURL}/run?version=3`;
    if (cleanedOptions.gateway?.id) {
      endpointUrl = `${this.#endpointURL}/ai-gateway/run?version=3`;
    }

    return await this.#fetcher.fetch(endpointUrl, fetchOptions);
  }

  /**
   * Generate fetch call for inputs with ReadableStream
   * */
  async #generateStreamFetch(
    inputs: Record<string, string | AiInputReadableStream>,
    options: AiOptions,
    cleanedOptions: CleanedAiOptions,
    model: string,
    streamKeys: string[]
  ): Promise<Response> {
    const streamKey = streamKeys[0] ?? '';
    const stream = streamKey ? inputs[streamKey] : null;
    const body = (stream as AiInputReadableStream).body;
    const contentType = (stream as AiInputReadableStream).contentType;

    if (cleanedOptions.gateway?.id) {
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
    const fetchOptions: RequestInit = {
      method: 'POST',
      body: body,
      headers: {
        ...options.sessionOptions?.extraHeaders,
        ...options.extraHeaders,
        'content-type': contentType,
        'cf-consn-sdk-version': '2.0.0',
        'cf-consn-model-id': `${options.prefix ? `${options.prefix}:` : ''}${model}`,
      },
    };
    if (options.signal) {
      fetchOptions.signal = options.signal;
    }

    // Fetch the additional input params
    const { [streamKey]: streamInput, ...userInputs } = inputs;

    // Construct query params
    // Append inputs with ai.run options that are passed to the inference request
    const query = {
      ...cleanedOptions,
      version: '3',
      userInputs: JSON.stringify({ ...userInputs }),
    };
    const aiEndpoint = new URL(`${this.#endpointURL}/run`);
    for (const [key, value] of Object.entries(query)) {
      aiEndpoint.searchParams.set(key, value as string);
    }

    return await this.#fetcher.fetch(aiEndpoint, fetchOptions);
  }

  /**
   * Generate call to open a websocket connection
   * */
  async #generateWebsocketFetch(
    inputs: object,
    options: AiOptions,
    cleanedOptions: CleanedAiOptions,
    model: string
  ): Promise<Response> {
    // Treat inputs as regular JS objects
    const body = JSON.stringify({
      inputs,
      options: cleanedOptions,
    });

    const fetchOptions: RequestInit = {
      headers: {
        ...options.sessionOptions?.extraHeaders,
        ...options.extraHeaders,
        'cf-consn-sdk-version': '2.0.0',
        'cf-consn-model-id': `${options.prefix ? `${options.prefix}:` : ''}${model}`,
        Upgrade: 'websocket',
      },
    };
    if (options.signal) {
      fetchOptions.signal = options.signal;
    }

    const aiEndpoint = new URL(`${this.#endpointURL}/run`);
    aiEndpoint.searchParams.set('version', '3');
    aiEndpoint.searchParams.set('body', body);

    return await this.#fetcher.fetch(aiEndpoint, fetchOptions);
  }

  run(
    model: string,
    inputs: Record<string, string | AiInputReadableStream>,
    options: AiOptions = {}
  ): Promise<AiRunOutput> {
    return this.#run(model, inputs, options, (result) => result);
  }

  runWithMetadata(
    model: string,
    inputs: Record<string, string | AiInputReadableStream>,
    options: AiOptions = {}
  ): Promise<AiRunResult<AiRunOutput>> {
    return this.#run(
      model,
      inputs,
      options,
      (result, logId, requestId, httpStatus) => ({
        result,
        logId,
        requestId,
        httpStatus,
      })
    );
  }

  async #run<Output>(
    model: string,
    inputs: Record<string, string | AiInputReadableStream>,
    options: AiOptions,
    formatResult: (
      result: AiRunOutput,
      logId: string | null,
      requestId: string | null,
      httpStatus: number
    ) => Output
  ): Promise<Output> {
    const websocket = options.websocket;
    const returnRawResponse = options.returnRawResponse || websocket;

    // This removes some unwanted options from getting sent in the body
    const cleanedOptions = (({
      prefix,
      extraHeaders,
      sessionOptions,
      signal,
      ...object
    }): CleanedAiOptions => object)(options);

    let res: Response;

    if (websocket) {
      res = await this.#generateWebsocketFetch(
        inputs,
        options,
        cleanedOptions,
        model
      );
    } else {
      /**
       * Inputs that contain a ReadableStream which will be sent directly to
       * the fetcher object along with other keys parsed as a query parameters
       * */
      const streamKeys = findReadableStreamKeys(inputs);

      if (streamKeys.length === 0) {
        res = await this.#generateFetch(inputs, options, cleanedOptions, model);
      } else if (streamKeys.length > 1) {
        throw new AiInternalError(
          `Multiple ReadableStreams are not supported. Found streams in keys: [${streamKeys.join(', ')}]`
        );
      } else {
        res = await this.#generateStreamFetch(
          inputs,
          options,
          cleanedOptions,
          model,
          streamKeys
        );
      }
    }

    const requestId = res.headers.get('cf-ai-req-id');
    const logId = res.headers.get('cf-aig-log-id');
    const httpStatus = res.status;
    const responseSequence = ++this.#responseSequence;
    this.lastRequestId = requestId;
    this.aiGatewayLogId = logId;
    this.lastRequestHttpStatusCode = httpStatus;
    this.lastRequestInternalStatusCode = null;

    if (returnRawResponse) {
      return formatResult(res, logId, requestId, httpStatus);
    }

    if (!res.ok || !res.body) {
      throw await this._parseError(res, (code): void => {
        if (this.#responseSequence === responseSequence) {
          this.lastRequestInternalStatusCode = code;
        }
      });
    }

    const contentType = res.headers.get('content-type');
    if (contentType === 'application/json') {
      return formatResult(
        (await res.json()) as object,
        logId,
        requestId,
        httpStatus
      );
    }

    return formatResult(res.body, logId, requestId, httpStatus);
  }

  /*
   * @deprecated this method is deprecated, do not use this
   */
  getLogs(): string[] {
    return [];
  }

  // TODO(soon): Can we use the # syntax here?
  // eslint-disable-next-line no-restricted-syntax
  private async _parseError(
    res: Response,
    setInternalStatusCode?: (code: number) => void
  ): Promise<InferenceUpstreamError> {
    const content = await res.text();
    const httpStatus = res.status || undefined;
    const logId = res.headers.get('cf-aig-log-id') ?? undefined;
    const headerRequestId = res.headers.get('cf-ai-req-id') ?? undefined;

    try {
      const parsedContent = JSON.parse(content) as AiError;
      if (parsedContent.internalCode) {
        setInternalStatusCode?.(parsedContent.internalCode);
        const defaultMessage =
          parsedContent.name && parsedContent.description
            ? `${parsedContent.name}: ${parsedContent.description}`
            : undefined;
        const detail =
          parsedContent.message && parsedContent.message !== defaultMessage
            ? parsedContent.message
            : parsedContent.description
              ? parsedContent.description
              : (parsedContent.message ?? content);
        return new InferenceUpstreamError(
          `${parsedContent.internalCode}: ${detail}`,
          parsedContent.name,
          {
            code: parsedContent.internalCode,
            httpStatus: parsedContent.httpCode ?? httpStatus,
            retryable: parsedContent.retryable,
            requestId: parsedContent.requestId ?? headerRequestId,
            logId,
            description: parsedContent.description,
          }
        );
      } else if (
        parsedContent.errors &&
        parsedContent.errors.length > 0 &&
        parsedContent.errors[0]
      ) {
        return new InferenceUpstreamError(
          `${parsedContent.errors[0].code}: ${parsedContent.errors[0].message}`,
          'InferenceUpstreamError',
          {
            code: parsedContent.errors[0].code,
            httpStatus: parsedContent.httpCode ?? httpStatus,
            retryable: parsedContent.retryable,
            requestId: parsedContent.requestId ?? headerRequestId,
            logId,
          }
        );
      } else {
        return new InferenceUpstreamError(content, 'InferenceUpstreamError', {
          httpStatus,
          requestId: headerRequestId,
          logId,
        });
      }
    } catch {
      return new InferenceUpstreamError(content, 'InferenceUpstreamError', {
        httpStatus,
        requestId: headerRequestId,
        logId,
      });
    }
  }

  async models(
    params: AiModelsSearchParams = {}
  ): Promise<AiModelsSearchObject[]> {
    const url = new URL(`${this.#endpointURL}/ai-api/models/search`);

    for (const [key, value] of Object.entries(params)) {
      url.searchParams.set(key, value.toString());
    }

    const res = await this.#fetcher.fetch(url, { method: 'GET' });

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

  toMarkdown(): ToMarkdownService;
  async toMarkdown(
    files: MarkdownDocument[],
    options?: ConversionRequestOptions
  ): Promise<ConversionResponse[]>;
  async toMarkdown(
    files: MarkdownDocument,
    options?: ConversionRequestOptions
  ): Promise<ConversionResponse>;
  toMarkdown(
    files?: MarkdownDocument | MarkdownDocument[],
    options?: ConversionRequestOptions
  ): ToMarkdownService | Promise<ConversionResponse | ConversionResponse[]> {
    // TODO(soon): Experimentally use RPC via `this.#fetcher.toMarkdown()`. Does not work correctly
    //   because `Blob` is not serializable; we'll need to fix that in the runtime first.
    const service = new ToMarkdownService(this.#fetcher);

    if (arguments.length < 1 || !files) return service;

    // NOTE(nunopereira): assuming type A = { name: string; blob: Blob }, 'files' here can be of type A | A[].
    // However, 'service.transform' has no overload that accepts that union, rather it has one overload for each variant.
    // We know the type of 'files' satisfies whatever type 'service.transform' expects and
    // instead it's Typescript that is failing to narrow the type, so just ignore this error.
    // @ts-expect-error unable to narrow type of file to either { name: string; blob: Blob } or { name: string; blob: Blob }[]
    return service.transform(files, options);
  }

  gateway(gatewayId: string, options?: { beta?: boolean }): AiGateway {
    // Use RPC if the `beta` flag is set or if the "experimental" compat flag is in use. Note that
    // this is explicitly structured to allow opting out with `{beta: false}`, even if
    // "experimental" is set, because currently `AbortSignal` does not work over RPC, and Gadgets
    // needs it (and sets "experimental" for other reasons).
    if (options?.beta ?? aiBindingExperimental) {
      return this.#fetcher.gateway(gatewayId);
    }
    return new AiGateway(this.#fetcher, gatewayId);
  }

  autorag(autoragId?: string): AutoRAG {
    if (aiBindingExperimental) {
      return this.#fetcher.autorag(autoragId);
    }
    return new AutoRAG(this.#fetcher, autoragId);
  }

  aiSearch(): AiSearchService {
    return this.#fetcher.aiSearch();
  }

  websearch(
    request: AiWebSearchRequest,
    params?: AiWebSearchParams
  ): Promise<Response> {
    return this.#fetcher.websearch(request, params);
  }
}

export default function makeBinding(env: { fetcher: Fetcher }): Ai {
  return new Ai(env.fetcher);
}
