// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { AiGateway, type GatewayOptions } from 'cloudflare-internal:aig-api'
import { AutoRAG } from 'cloudflare-internal:autorag-api'
import {
  type ConversionRequestOptions,
  type ConversionResponse,
  type MarkdownDocument,
  ToMarkdownService,
} from 'cloudflare-internal:to-markdown-api'

interface Fetcher {
  fetch: typeof fetch
}

interface AiError {
  internalCode: number
  message: string
  name: string
  description: string
  errors?: Array<{ code: number; message: string }>
}

export type SessionOptions = {
  // Deprecated, do not use this
  extraHeaders?: object
}

export type AiOptions = {
  gateway?: GatewayOptions
  websocket?: boolean
  /** If true it will return a Response object */
  returnRawResponse?: boolean
  prefix?: string
  extraHeaders?: object
  /*
   * @deprecated this option is deprecated, do not use this
   */
  sessionOptions?: SessionOptions
}

export type AiInputReadableStream = {
  body: ReadableStream | FormData
  contentType: string
}

export type AiModelsSearchParams = {
  author?: string
  hide_experimental?: boolean
  page?: number
  per_page?: number
  search?: string
  source?: number
  task?: string
}

export type AiModelsSearchObject = {
  id: string
  source: number
  name: string
  description: string
  task: {
    id: string
    name: string
    description: string
  }
  tags: string[]
  properties: {
    property_id: string
    value: string
  }[]
}

export class InferenceUpstreamError extends Error {
  constructor(message: string, name = 'InferenceUpstreamError') {
    super(message)
    this.name = name
  }
}

export class AiInternalError extends Error {
  constructor(message: string, name = 'AiInternalError') {
    super(message)
    this.name = name
  }
}

function isReadableStream(obj: unknown): obj is ReadableStream {
  return obj instanceof ReadableStream
}

function isFormData(obj: unknown): obj is FormData {
  return obj instanceof FormData
}

/**
 * Find keys in inputs that have a ReadableStream
 * */
function findReadableStreamKeys(
  inputs: Record<string, unknown>,
): Array<string> {
  const readableStreamKeys: Array<string> = []

  for (const [key, value] of Object.entries(inputs)) {
    // Check if value has a body property that's a ReadableStream
    const hasReadableStreamBody =
      value &&
      typeof value === 'object' &&
      'body' in value &&
      (isReadableStream(value.body) || isFormData(value.body))

    if (hasReadableStreamBody || isReadableStream(value) || isFormData(value)) {
      readableStreamKeys.push(key)
    }
  }

  return readableStreamKeys
}

export class Ai {
  #fetcher: Fetcher
  #options: AiOptions = {}
  #endpointURL = 'https://workers-binding.ai'
  lastRequestId: string | null = null
  aiGatewayLogId: string | null = null
  lastRequestHttpStatusCode: number | null = null
  lastRequestInternalStatusCode: number | null = null

  constructor(fetcher: Fetcher) {
    this.#fetcher = fetcher
  }

  async fetch(input: RequestInfo | URL, init?: RequestInit): Promise<Response> {
    return this.#fetcher.fetch(input, init)
  }

  /**
   * Generate fetch call for JSON inputs
   * */
  async #generateFetch(
    inputs: object,
    options: AiOptions,
    model: string,
  ): Promise<Response> {
    // Treat inputs as regular JS objects
    const body = JSON.stringify({
      inputs,
      options,
    })

    const fetchOptions = {
      method: 'POST',
      body: body,
      headers: {
        ...this.#options.sessionOptions?.extraHeaders,
        ...this.#options.extraHeaders,
        'content-type': 'application/json',
        'cf-consn-sdk-version': '2.0.0',
        'cf-consn-model-id': `${this.#options.prefix ? `${this.#options.prefix}:` : ''}${model}`,
      },
    }

    let endpointUrl = `${this.#endpointURL}/run?version=3`
    if (options.gateway?.id) {
      endpointUrl = `${this.#endpointURL}/ai-gateway/run?version=3`
    }

    return await this.#fetcher.fetch(endpointUrl, fetchOptions)
  }

  /**
   * Generate fetch call for inputs with ReadableStream
   * */
  async #generateStreamFetch(
    inputs: Record<string, string | AiInputReadableStream>,
    options: AiOptions,
    model: string,
    streamKeys: string[],
  ): Promise<Response> {
    const streamKey = streamKeys[0] ?? ''
    const stream = streamKey ? inputs[streamKey] : null
    const body = (stream as AiInputReadableStream).body
    const contentType = (stream as AiInputReadableStream).contentType

    if (options.gateway?.id) {
      throw new AiInternalError(
        'AI Gateway does not support ReadableStreams yet.',
      )
    }

    // Make sure user has supplied the Content-Type
    // This allows AI binding to treat the ReadableStream correctly
    if (!contentType) {
      throw new AiInternalError(
        'Content-Type is required with ReadableStream inputs',
      )
    }

    // Pass single ReadableStream in request body
    const fetchOptions = {
      method: 'POST',
      body: body,
      headers: {
        ...this.#options.sessionOptions?.extraHeaders,
        ...this.#options.extraHeaders,
        'content-type': contentType,
        'cf-consn-sdk-version': '2.0.0',
        'cf-consn-model-id': `${this.#options.prefix ? `${this.#options.prefix}:` : ''}${model}`,
      },
    }

    // Fetch the additional input params
    const { [streamKey]: streamInput, ...userInputs } = inputs

    // Construct query params
    // Append inputs with ai.run options that are passed to the inference request
    const query = {
      ...options,
      version: '3',
      userInputs: JSON.stringify({ ...userInputs }),
    }
    const aiEndpoint = new URL(`${this.#endpointURL}/run`)
    for (const [key, value] of Object.entries(query)) {
      aiEndpoint.searchParams.set(key, value as string)
    }

    return await this.#fetcher.fetch(aiEndpoint, fetchOptions)
  }

  /**
   * Generate call to open a websocket connection
   * */
  async #generateWebsocketFetch(
    inputs: object,
    options: AiOptions,
    model: string,
  ): Promise<Response> {
    // Treat inputs as regular JS objects
    const body = JSON.stringify({
      inputs,
      options,
    })

    const fetchOptions = {
      headers: {
        ...this.#options.sessionOptions?.extraHeaders,
        ...this.#options.extraHeaders,
        'cf-consn-sdk-version': '2.0.0',
        'cf-consn-model-id': `${this.#options.prefix ? `${this.#options.prefix}:` : ''}${model}`,
        Upgrade: 'websocket',
      },
    }

    const aiEndpoint = new URL(`${this.#endpointURL}/run`)
    aiEndpoint.searchParams.set('version', '3')
    aiEndpoint.searchParams.set('body', body)

    return await this.#fetcher.fetch(aiEndpoint, fetchOptions)
  }

  async run(
    model: string,
    inputs: Record<string, string | AiInputReadableStream>,
    options: AiOptions = {},
  ): Promise<Response | ReadableStream<Uint8Array> | object | null> {
    this.#options = options
    this.lastRequestId = ''

    // This removes some unwanted options from getting sent in the body
    const cleanedOptions = (({
      prefix,
      extraHeaders,
      sessionOptions,
      ...object
    }): object => object)(this.#options)

    let res: Response

    if (this.#options.websocket) {
      res = await this.#generateWebsocketFetch(inputs, options, model)
    } else {
      /**
       * Inputs that contain a ReadableStream which will be sent directly to
       * the fetcher object along with other keys parsed as a query parameters
       * */
      const streamKeys = findReadableStreamKeys(inputs)

      if (streamKeys.length === 0) {
        res = await this.#generateFetch(inputs, cleanedOptions, model)
      } else if (streamKeys.length > 1) {
        throw new AiInternalError(
          `Multiple ReadableStreams are not supported. Found streams in keys: [${streamKeys.join(', ')}]`,
        )
      } else {
        res = await this.#generateStreamFetch(
          inputs,
          options,
          model,
          streamKeys,
        )
      }
    }

    this.lastRequestId = res.headers.get('cf-ai-req-id')
    this.aiGatewayLogId = res.headers.get('cf-aig-log-id')
    this.lastRequestHttpStatusCode = res.status

    if (this.#options.returnRawResponse || this.#options.websocket) {
      return res
    }

    if (!res.ok || !res.body) {
      throw await this._parseError(res)
    }

    const contentType = res.headers.get('content-type')
    if (contentType === 'application/json') {
      return (await res.json()) as object
    }

    return res.body
  }

  /*
   * @deprecated this method is deprecated, do not use this
   */
  getLogs(): string[] {
    return []
  }

  // TODO(soon): Can we use the # syntax here?
  // eslint-disable-next-line no-restricted-syntax
  private async _parseError(res: Response): Promise<InferenceUpstreamError> {
    const content = await res.text()

    try {
      const parsedContent = JSON.parse(content) as AiError
      if (parsedContent.internalCode) {
        this.lastRequestInternalStatusCode = parsedContent.internalCode
        return new InferenceUpstreamError(
          `${parsedContent.internalCode}: ${parsedContent.description}`,
          parsedContent.name,
        )
      } else if (
        parsedContent.errors &&
        parsedContent.errors.length > 0 &&
        parsedContent.errors[0]
      ) {
        return new InferenceUpstreamError(
          `${parsedContent.errors[0].code}: ${parsedContent.errors[0].message}`,
        )
      } else {
        return new InferenceUpstreamError(content)
      }
    } catch {
      return new InferenceUpstreamError(content)
    }
  }

  async models(
    params: AiModelsSearchParams = {},
  ): Promise<AiModelsSearchObject[]> {
    const url = new URL(`${this.#endpointURL}/ai-api/models/search`)

    for (const [key, value] of Object.entries(params)) {
      url.searchParams.set(key, value.toString())
    }

    const res = await this.#fetcher.fetch(url, { method: 'GET' })

    switch (res.status) {
      case 200: {
        const data = (await res.json()) as { result: AiModelsSearchObject[] }
        return data.result
      }
      default: {
        const data = (await res.json()) as { errors: { message: string }[] }

        throw new AiInternalError(data.errors[0]?.message || 'Internal Error')
      }
    }
  }

  toMarkdown(): ToMarkdownService
  async toMarkdown(
    files: MarkdownDocument[],
    options?: ConversionRequestOptions,
  ): Promise<ConversionResponse[]>
  async toMarkdown(
    files: MarkdownDocument,
    options?: ConversionRequestOptions,
  ): Promise<ConversionResponse>
  toMarkdown(
    files?: MarkdownDocument | MarkdownDocument[],
    options?: ConversionRequestOptions,
  ): ToMarkdownService | Promise<ConversionResponse | ConversionResponse[]> {
    const service = new ToMarkdownService(this.#fetcher)

    if (arguments.length < 1 || !files) return service

    // NOTE(nunopereira): assuming type A = { name: string; blob: Blob }, 'files' here can be of type A | A[].
    // However, 'service.transform' has no overload that accepts that union, rather it has one overload for each variant.
    // We know the type of 'files' satisfies whatever type 'service.transform' expects and
    // instead it's Typescript that is failing to narrow the type, so just ignore this error.
    // @ts-expect-error unable to narrow type of file to either { name: string; blob: Blob } or { name: string; blob: Blob }[]
    return service.transform(files, options)
  }

  gateway(gatewayId: string): AiGateway {
    return new AiGateway(this.#fetcher, gatewayId)
  }

  autorag(autoragId?: string): AutoRAG {
    return new AutoRAG(this.#fetcher, autoragId)
  }
}

export default function makeBinding(env: { fetcher: Fetcher }): Ai {
  return new Ai(env.fetcher)
}
